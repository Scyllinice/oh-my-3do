#!/usr/bin/env python3
"""Convert a short silent video preview for the menu's RAM player."""
import argparse
import binascii
from pathlib import Path
import shutil
import struct
import subprocess


def convert(source, target, fps=6, frames=24, start=0):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="USB previews/Game Title.3dv")
    parser.add_argument("--fps", type=int, default=6)
    parser.add_argument("--frames", type=int, default=24)
    parser.add_argument("--start", type=float, default=0)
    args = parser.parse_args()
    try:
        convert(args.source, args.output, args.fps, args.frames, args.start)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
