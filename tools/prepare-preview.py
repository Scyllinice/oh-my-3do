#!/usr/bin/env python3
"""Prepare a bounded RAM video preview, with optional synchronized audio."""
import argparse
import binascii
import json
import math
import zlib
from pathlib import Path
import shutil
import struct
import subprocess


def convert_legacy(source, target, fps=6, frames=24, start=0):
    if not source.is_file() or not 1 <= fps <= 12 or not 1 <= frames <= 32 or not 0 <= start <= 86400:
        raise ValueError("Use an existing input, 1-12 fps, 1-32 frames and start 0-86400 seconds")
    if source.resolve() == target.resolve() or target.suffix.lower() != ".3dv":
        raise ValueError("Choose a different output file ending in .3dv")
    executable = shutil.which("ffmpeg")
    if not executable:
        raise ValueError("ffmpeg is required; rebuild the development container")
    result = subprocess.run([executable, "-v", "error", "-nostdin", "-ss", str(start), "-i", str(source),
        "-an", "-sn", "-vf", f"fps={fps},scale=64:80:force_original_aspect_ratio=decrease:flags=area,pad=64:80:(ow-iw)/2:(oh-ih)/2:color=0x081021",
        "-frames:v", str(frames), "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"], check=True, stdout=subprocess.PIPE)
    raw = result.stdout
    if not raw or len(raw) % (64*80*3) or len(raw) > 32*64*80*3:
        raise ValueError("Video did not produce complete bounded frames")
    pixels = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i:i+3]
        pixels += struct.pack(">H", ((r>>3)<<10)|((g>>3)<<5)|(b>>3))
    count = len(raw)//(64*80*3)
    data = b"OM3DVID1"+struct.pack(">HHIIHH",64,80,len(pixels),binascii.crc32(pixels),count,fps)+pixels+b"END3DV01"
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name+".tmp")
    temporary.write_bytes(data)
    temporary.replace(target)
    print(f"Created {target}: {count} frames at {fps} fps, {len(pixels)//1024} KiB, silent")


MAX_BYTES = 1024 * 1024
MAX_SECONDS = 120
MAX_FRAMES = 720


def rgb555(raw):
    pixels = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i:i+3]
        pixels += struct.pack('>H', ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))
    return pixels


def convert(source, target, fps=6, frames=None, start=0, duration=None, sound=True):
    # Explicit --frames retains the old silent format for old boot.iso builds.
    if frames is not None:
        if duration is not None:
            raise ValueError('Use --frames for legacy video or --duration for the new player, not both')
        return convert_legacy(source, target, fps, frames, start)
    source, target = Path(source), Path(target)
    if (not source.is_file() or not 1 <= fps <= 12 or not 0 <= start <= 86400 or
            not math.isfinite(start) or target.suffix.lower() != '.3dv' or source.resolve() == target.resolve()):
        raise ValueError('Choose an existing input, a separate .3dv output, 1-12 fps, and a valid start time')
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if not ffmpeg or not ffprobe:
        raise ValueError('FFmpeg and ffprobe are required on PATH')
    probe = subprocess.run([ffprobe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(source)],
                           check=True, stdout=subprocess.PIPE)
    info = json.loads(probe.stdout)
    if not any(s.get('codec_type') == 'video' for s in info.get('streams', [])):
        raise ValueError('Input has no video stream')
    full = duration is None or str(duration).lower() == 'full'
    try:
        available = float(info.get('format', {}).get('duration', 'nan')) - start
    except (ValueError, TypeError):
        available = float('nan')
    requested = available if full else float(duration)
    if full and (not math.isfinite(requested) or requested > MAX_SECONDS):
        requested = 30
        print('Full clip exceeds the 120-second format limit or has unknown duration; trying 30 seconds.')
    if not math.isfinite(requested) or not 0 < requested <= MAX_SECONDS:
        raise ValueError('Preview duration must be greater than zero and at most 120 seconds')
    if math.isfinite(available):
        requested = min(requested, available)
    if requested <= 0:
        raise ValueError('Start position is beyond the end of the source')
    has_audio = sound and any(s.get('codec_type') == 'audio' for s in info.get('streams', []))
    attempts = [requested]
    if full and requested > 30:
        attempts.append(30)
    selected = None
    for seconds in attempts:
        # Preserve more of a full clip at 4 fps before falling back to 30s.
        minimum = min(fps, 4 if full and seconds > 30 else 2)
        for rate in range(fps, minimum-1, -1):
            limit = min(MAX_FRAMES, math.ceil(seconds * rate))
            if seconds * rate > MAX_FRAMES:
                continue
            raw = subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
                '-map', '0:v:0', '-an', '-sn', '-t', str(seconds), '-vf',
                f'fps={rate},scale=64:80:force_original_aspect_ratio=decrease:flags=area,pad=64:80:(ow-iw)/2:(oh-ih)/2:color=0x081021',
                '-frames:v', str(limit), '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'],
                check=True, stdout=subprocess.PIPE).stdout
            if not raw or len(raw) % (64*80*3) or len(raw) > MAX_FRAMES*64*80*3:
                raise ValueError('Video did not produce complete bounded frames')
            count = len(raw)//(64*80*3)
            blocks = [zlib.compress(rgb555(raw[i:i+64*80*3]), 9) for i in range(0, len(raw), 64*80*3)]
            offset = (count+1)*4
            offsets = [offset]
            for block in blocks:
                offset += len(block)
                offsets.append(offset)
            audio_size = ((math.ceil(count*11025/rate)+3)//4)*4 if has_audio else 0
            if offset + audio_size <= MAX_BYTES:
                video = b''.join(struct.pack('>I', n) for n in offsets) + b''.join(blocks)
                selected = (video, count, rate, audio_size, count/rate)
                break
        if selected:
            break
        print(f'{seconds:g}-second clip exceeds the 1 MiB RAM payload budget.')
    if not selected:
        raise ValueError('Preview exceeds the RAM budget; use a shorter --duration or lower --fps')
    video, count, rate, audio_size, seconds = selected
    audio = b''
    if audio_size:
        audio = subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
            '-map', '0:a:0', '-vn', '-sn', '-t', str(seconds), '-ac', '1', '-ar', '11025', '-af',
            f'volume=0.8,afade=t=in:d=0.01,afade=t=out:st={max(0,seconds-0.01)}:d=0.01',
            '-f', 's8', 'pipe:1'], check=True, stdout=subprocess.PIPE).stdout
        audio = audio[:audio_size].ljust(audio_size, b'\0')
    payload = video + audio
    header = b'OM3DVID2'+struct.pack('>HHIIHHIIII',64,80,len(payload),binascii.crc32(payload),count,rate,
                                  len(video),len(audio),11025 if audio else 0,len(audio))
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name+'.tmp')
    try:
        temporary.write_bytes(header+payload+b'END3DV02')
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    print(f'Created {target}: {seconds:.2f} seconds, {count} frames at {rate} fps, '
          f'{len(payload)/1024:.1f} KiB RAM; ' + ('11025 Hz mono audio' if audio else 'silent'))
    print('Requires the v0.16.0+ menu. Playback is entirely from RAM; original source unchanged.')
    return dict(seconds=seconds, frames=count, fps=rate, bytes=len(payload), audio_bytes=len(audio))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="USB .ohmy3do/previews/Game Title.3dv (or legacy previews/)")
    parser.add_argument("--fps", type=int, default=6)
    parser.add_argument("--frames", type=int, help="legacy silent video: 1-32 frames")
    parser.add_argument("--duration", default=None, help="full (default, with 30s RAM fallback) or seconds, up to 120")
    parser.add_argument("--silent", action="store_true", help="omit audio")
    parser.add_argument("--start", type=float, default=0)
    args = parser.parse_args()
    try:
        convert(args.source, args.output, args.fps, args.frames, args.start, args.duration, not args.silent)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
