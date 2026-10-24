#!/usr/bin/env python3
"""Prepare a video preview with optional synchronized audio: streamed 320x240
(OM3DVID7, the default, as the desktop app makes them) or the older 160x120
clip that fits in RAM (OM3DVID5, --format 5)."""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


CLIP_CAP_SECONDS = 15
WIDTH, HEIGHT, FPS = 160, 120, 30
V5_BYTES = 1400 * 1024  # video + audio; fits the DRAM + VRAM a console has free
V5_FRAMES = CLIP_CAP_SECONDS * FPS
V5_PACKET = 1200 * 12   # worst-case packet: every block GRAD with literal colours


# OM3DVID7 (src/preview_v7.h): the whole clip up to a minute, streamed at
# about 200 KB/s. The same settings as the desktop app (desktop/src/media.rs).
STREAM_WIDTH, STREAM_HEIGHT = 320, 240
STREAM_MAX_SECONDS = 60
STREAM_PARAMS = dict(rate=200*1024, read_rate=300*1024, slot_bytes=768*1024, max_chunk=128*1024,
                     chunk_frames=10, entries4=8192, entries2=4096, cycle_cap=230000)


class StreamParams(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in STREAM_PARAMS]


def library_path(stem, directory=None):
    """A native library beside this tool, in build/media-native, or built now."""
    name = (stem+'-win64.dll' if os.name == 'nt' else stem+'.dylib' if sys.platform == 'darwin' else stem+'.so')
    folder = Path(__file__).resolve().parent
    candidates = [Path(directory)/name] if directory else [folder/name, folder.parent/'build/media-native'/name]
    path = next((p for p in candidates if p.is_file()), None)
    builder = folder/'build-preview-encoder.py'
    if not path and not directory and builder.is_file() and shutil.which('cc'):
        # Source checkout with a C compiler: build the libraries once.
        print('Building the preview encoder libraries...')
        subprocess.run([sys.executable, str(builder)], check=True, stdout=subprocess.DEVNULL)
        path = next((p for p in candidates if p.is_file()), None)
    return path


def stream_encoder(directory=None):
    """Load the streamed-preview encoder (tools/preview7-encoder.c), or None."""
    path = library_path('preview7-encoder', directory)
    if not path:
        return None
    library = ctypes.CDLL(str(path.resolve()))
    library.oh3do_preview7_abi.restype = ctypes.c_uint32
    if library.oh3do_preview7_abi() != 1:
        raise ValueError('Unsupported streamed preview encoder ABI: ' + str(path))
    library.oh3do_preview7_workspace.restype = ctypes.c_uint64
    library.oh3do_preview7_workspace.argtypes = (ctypes.c_uint32,)
    library.oh3do_preview7_begin.argtypes = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32)
    library.oh3do_preview7_frame.argtypes = (ctypes.c_void_p, ctypes.c_char_p)
    library.oh3do_preview7_encode.restype = ctypes.c_int64
    library.oh3do_preview7_encode.argtypes = (ctypes.c_void_p, ctypes.POINTER(StreamParams), ctypes.c_void_p, ctypes.c_uint64,
                                              ctypes.c_void_p, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32))
    return library


def encode_stream(library, frames, reserve, audio=None):
    """An OM3DVID7 file from 320x240 RGB24 frames (any iterable of frames, up
    to reserve of them) and signed 8-bit 11025 Hz sound for exactly those
    frames, or None. Returns (file bytes, frame count)."""
    workspace = ctypes.create_string_buffer(library.oh3do_preview7_workspace(reserve))
    if library.oh3do_preview7_begin(workspace, len(workspace), reserve):
        raise ValueError('Preview encoder setup failed')
    count = 0
    for frame in frames:
        if count == reserve or library.oh3do_preview7_frame(workspace, frame):
            raise ValueError('Too many or invalid preview frames')
        count += 1
    if not count:
        raise ValueError('Video did not produce complete frames')
    samples = count*11025//FPS
    if audio is not None and len(audio) != samples:
        raise ValueError('Preview sound does not match its frames')
    capacity = STREAM_PARAMS['rate']*count//FPS*11//10 + (2 << 20)
    out, stats = ctypes.create_string_buffer(capacity), (ctypes.c_uint32*8)()
    n = library.oh3do_preview7_encode(workspace, ctypes.byref(StreamParams(**STREAM_PARAMS)), audio, samples if audio else 0,
                                      out, capacity, stats)
    if n < 0:
        raise ValueError(f'Preview encoder failed (error {n})')
    return out.raw[:n], count


def convert_stream(source, target, start=0, duration=None, sound=True, encoder=None):
    """OM3DVID7: 320x240 at 30 fps, the whole source up to a minute, streamed."""
    source, target = Path(source), Path(target)
    if (not source.is_file() or target.suffix.lower() != '.3dv' or source.resolve() == target.resolve() or
            not math.isfinite(start) or not 0 <= start <= 86400):
        raise ValueError('Choose an existing source, a separate .3dv output and a valid start time')
    library = encoder or stream_encoder()
    if not library:
        raise ValueError('The preview encoder library is missing; run build-preview-encoder.py')
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if not ffmpeg or not ffprobe:
        raise ValueError('FFmpeg and ffprobe are required on PATH')
    info = json.loads(subprocess.run([ffprobe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(source)],
                                     check=True, stdout=subprocess.PIPE).stdout)
    if not any(s.get('codec_type') == 'video' for s in info.get('streams', [])):
        raise ValueError('Source has no video stream')
    sound = sound and any(s.get('codec_type') == 'audio' for s in info.get('streams', []))
    wanted = STREAM_MAX_SECONDS if duration is None or str(duration).lower() == 'full' else float(duration)
    if not math.isfinite(wanted) or wanted <= 0:
        raise ValueError('Preview duration must be a finite value greater than zero')
    try:
        available = float(info.get('format', {}).get('duration', 'nan')) - start
    except (ValueError, TypeError):
        available = float('nan')
    seconds = min(wanted, STREAM_MAX_SECONDS, available) if math.isfinite(available) else min(wanted, STREAM_MAX_SECONDS)
    if seconds <= 0:
        raise ValueError('Start position is beyond the end of the source')
    reserve = min(STREAM_MAX_SECONDS*FPS, math.ceil(seconds*FPS)+2)
    size = STREAM_WIDTH*STREAM_HEIGHT*3
    reader = subprocess.Popen([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
        '-map', '0:v:0', '-an', '-sn', '-t', str(seconds), '-vf',
        f'fps={FPS},scale={STREAM_WIDTH}:{STREAM_HEIGHT}:force_original_aspect_ratio=decrease:flags=area,'
        f'pad={STREAM_WIDTH}:{STREAM_HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=0x081021',
        '-frames:v', str(reserve), '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'], stdout=subprocess.PIPE)
    frames = iter(lambda: reader.stdout.read(size), b'')
    try:
        whole = (frame for frame in frames if len(frame) == size)
        audio = None
        if sound:
            # The sound needs the frame count, so the frames are kept first.
            kept = list(whole)
            count = len(kept)
            audio = subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
                '-map', '0:a:0', '-vn', '-sn', '-t', str(count/FPS), '-ac', '1', '-ar', '11025', '-af',
                f'volume=0.8,afade=t=in:d=0.01,afade=t=out:st={max(0, count/FPS-0.01)}:d=0.01',
                '-f', 's8', 'pipe:1'], check=True, stdout=subprocess.PIPE).stdout
            audio = audio[:count*11025//FPS].ljust(count*11025//FPS, b'\0')
            whole = kept
        data, count = encode_stream(library, whole, reserve, audio)
    finally:
        reader.stdout.close()
        if reader.wait():
            raise ValueError('FFmpeg failed to read the source video')
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name+'.tmp')
    try:
        temporary.write_bytes(data)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    print(f'Created {target}: {count/FPS:.2f} seconds, {count} frames at {FPS} fps, {len(data)/1024:.1f} KiB streamed'
          + ('; 11025 Hz mono audio' if audio else '; silent'))
    return dict(seconds=count/FPS, frames=count, fps=FPS, bytes=len(data), audio_bytes=len(audio or b''))


def native_encoder(directory=None):
    """Load the OM3DVID5 encoder library beside this tool (or an explicit test dir)."""
    path = library_path('preview-encoder', directory)
    if not path:
        return None
    library = ctypes.CDLL(str(path.resolve()))
    library.oh3do_preview_abi.restype = ctypes.c_uint
    if library.oh3do_preview_abi() != 2:
        raise ValueError('Unsupported preview encoder ABI: ' + str(path))
    encode = library.oh3do_preview_encode
    encode.argtypes = (ctypes.c_char_p, ctypes.c_uint, ctypes.c_double, ctypes.c_uint, ctypes.c_double,
                       ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p)
    encode.restype = ctypes.c_long
    return encode


def encode_v5(frames, count, budget, encode, decoded=None):
    """Smallest lambda (best quality) whose video section fits budget bytes.

    Size jumps between nearby lambdas, so the rest of the budget is filled by
    encoding the first frames with the next smaller lambda."""
    out = ctypes.create_string_buffer((count+1)*4 + count*V5_PACKET)
    def size(first, split, rest, keep=None):
        n = encode(frames, count, first, split, rest, out, len(out), keep)
        if n < 0:
            raise ValueError(f'Preview encoder failed (error {n})')
        return n
    # Bracket from a typical lambda, widening by 4x, then bisect to 2%.
    low, high = 0.5, 0.5
    if size(high, 0, high) <= budget:
        while low > 0.01:
            low = max(0.01, low/4)
            if size(low, 0, low) > budget:
                break
            high = low
        else:
            low = high  # best quality fits
    else:
        while True:
            low, high = high, high*4
            if high > 100000:
                raise ValueError('Preview video cannot fit the RAM budget; choose a shorter duration')
            if size(high, 0, high) <= budget:
                break
    while high/low > 1.02:
        middle = math.sqrt(low*high)
        if size(middle, 0, middle) <= budget:
            high = middle
        else:
            low = middle
    split = 0
    if low < high:
        fit, over = 0, count+1
        while over-fit > 1:
            middle = (fit+over)//2
            if size(low, middle, high) <= budget:
                fit = middle
            else:
                over = middle
        split = fit
    n = size(low, split, high, decoded)
    return out.raw[:n]


def word_sum(data):
    """OM3DVID5 payload check (see src/usb_media.c): over big-endian words,
    a += w; b += a; result a ^ (b rotated by 16), modulo 2**32."""
    a = b = 0
    for (w,) in struct.iter_unpack('>I', data):
        a = (a + w) & 0xffffffff
        b = (b + a) & 0xffffffff
    return a ^ (((b << 16) | (b >> 16)) & 0xffffffff)


def audio_bytes(count):
    """Signed 8-bit 11025 Hz samples for count frames, padded to a word."""
    return (((count*11025 + FPS - 1)//FPS) + 3) & ~3


def v5_container(video, count, audio):
    """Frame table, audio, video packets, zero padding to a word boundary.
    The menu reads the audio first, then splits the packets across its memory."""
    table = (count + 1) * 4
    payload = video[:table] + audio + video[table:] + bytes(-len(video) % 4)
    return (b'OM3DVID5' + struct.pack('>HHIIHHIIII', WIDTH, HEIGHT, len(payload), word_sum(payload), count, FPS,
                                      len(video), len(audio), 11025 if audio else 0, len(audio)) + payload + b'END3DV05')


def convert(source, target, start=0, duration=None, sound=True, budget=V5_BYTES, encoder=None):
    """OM3DVID5: block codec, 160x120 at 30 fps, up to 15 s, sized to budget bytes."""
    source, target = Path(source), Path(target)
    if (not source.is_file() or target.suffix.lower() != '.3dv' or source.resolve() == target.resolve() or
            not math.isfinite(start) or not 0 <= start <= 86400):
        raise ValueError('Choose an existing source, a separate .3dv output and a valid start time')
    encode = encoder or native_encoder()
    if not encode:
        raise ValueError('The preview encoder library is missing; run build-preview-encoder.py')
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if not ffmpeg or not ffprobe:
        raise ValueError('FFmpeg and ffprobe are required on PATH')
    info = json.loads(subprocess.run([ffprobe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(source)],
                                     check=True, stdout=subprocess.PIPE).stdout)
    if not any(s.get('codec_type') == 'video' for s in info.get('streams', [])):
        raise ValueError('Source has no video stream')
    sound = sound and any(s.get('codec_type') == 'audio' for s in info.get('streams', []))
    wanted = CLIP_CAP_SECONDS if duration is None or str(duration).lower() == 'full' else float(duration)
    if not math.isfinite(wanted) or wanted <= 0:
        raise ValueError('Preview duration must be a finite value greater than zero')
    if wanted > CLIP_CAP_SECONDS:
        print('Preview length capped at 15 seconds.')
    try:
        available = float(info.get('format', {}).get('duration', 'nan')) - start
    except (ValueError, TypeError):
        available = float('nan')
    seconds = min(wanted, CLIP_CAP_SECONDS)
    if math.isfinite(available):
        seconds = min(seconds, available)
    if seconds <= 0:
        raise ValueError('Start position is beyond the end of the source')
    limit = min(V5_FRAMES, math.ceil(seconds*FPS))
    raw = subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
        '-map', '0:v:0', '-an', '-sn', '-t', str(seconds), '-vf',
        f'fps={FPS},scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease:flags=area,'
        f'pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=0x081021',
        '-frames:v', str(limit), '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'], check=True, stdout=subprocess.PIPE).stdout
    frame = WIDTH*HEIGHT*3
    if not raw or len(raw) % frame or len(raw) > limit*frame:
        raise ValueError('Video did not produce complete bounded frames')
    count = len(raw)//frame
    audio_size = audio_bytes(count) if sound else 0
    if budget - audio_size < (count+1)*4 + count*8:
        raise ValueError('Preview RAM budget is too small for this clip')
    video = encode_v5(raw, count, budget - audio_size, encode)
    seconds = count/FPS
    audio = b''
    if sound:
        audio = subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-ss', str(start), '-i', str(source),
            '-map', '0:a:0', '-vn', '-sn', '-t', str(seconds), '-ac', '1', '-ar', '11025', '-af',
            f'volume=0.8,afade=t=in:d=0.01,afade=t=out:st={max(0,seconds-0.01)}:d=0.01',
            '-f', 's8', 'pipe:1'], check=True, stdout=subprocess.PIPE).stdout[:audio_size].ljust(audio_size, b'\0')
    data = v5_container(video, count, audio)
    payload_bytes = len(data) - 48  # 40-byte header, 8-byte end marker
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name+'.tmp')
    try:
        temporary.write_bytes(data)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    print(f'Created {target}: {seconds:.2f} seconds, {count} frames at {FPS} fps, {payload_bytes/1024:.1f} KiB RAM'
          + ('; 11025 Hz mono audio' if audio else '; silent'))
    return dict(seconds=seconds, frames=count, fps=FPS, bytes=payload_bytes, audio_bytes=len(audio))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="the game's .ohmy3do/NAME.3dv")
    parser.add_argument("--format", type=int, choices=(5, 7), default=7,
                        help="7: streamed 320x240, the whole clip up to a minute (default); 5: 160x120 in RAM, up to 15 s")
    parser.add_argument("--duration", default=None, help="maximum seconds (default: full, up to 60; format 5: 15)")
    parser.add_argument("--silent", action="store_true", help="omit audio")
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--budget-kib", type=int, default=V5_BYTES//1024,
                        help="format 5: RAM for video and audio together (default: %(default)s)")
    args = parser.parse_args()
    try:
        if args.format == 7:
            convert_stream(args.source, args.output, args.start, args.duration, not args.silent)
            return
        if not 64 <= args.budget_kib <= 4096:
            raise ValueError("Choose a budget of 64-4096 KiB")
        convert(args.source, args.output, args.start, args.duration, not args.silent, args.budget_kib*1024)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
