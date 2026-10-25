#!/usr/bin/env python3
"""Losslessly combine a CD-quality WAV/CUE audio disc into one BIN/CUE pair.

Original files are never modified. The output directory must not already exist.
Track indexes (including recorded pregaps) are preserved, without resampling.
Console playback depends on the installed ODE firmware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import wave


def digest(path):
    with path.open("rb") as stream:
        result = hashlib.sha1()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
        return result.hexdigest()


def sectors(value):
    minute, second, frame = map(int, value.split(":"))
    if second >= 60 or frame >= 75:
        raise ValueError("Invalid CD time: " + value)
    return (minute * 60 + second) * 75 + frame


def msf(value):
    minute, rest = divmod(value, 60 * 75)
    second, frame = divmod(rest, 75)
    return f"{minute:02}:{second:02}:{frame:02}"


def inspect(source):
    tracks = []
    current = None
    metadata = []
    for line in source.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line:
            continue
        match = re.fullmatch(r'FILE "([^"\r\n]+)" WAVE', line, re.I)
        if match:
            name = match[1]
            if any(c in name for c in "/\\:") or name in (".", ".."):
                raise ValueError("WAV must be beside its CUE: " + name)
            current = {"path": source.parent / name, "indexes": [], "metadata": []}
            tracks.append(current)
            continue
        match = re.fullmatch(r"TRACK (\d{1,2}) AUDIO", line, re.I)
        if match:
            if current is None or "number" in current:
                raise ValueError("Expected one AUDIO track per WAV")
            current["number"] = int(match[1])
            continue
        match = re.fullmatch(r"INDEX (\d{2}) (\d{2,3}:\d{2}:\d{2})", line, re.I)
        if match and current is not None and "number" in current:
            current["indexes"].append((int(match[1]), sectors(match[2])))
            continue
        if re.fullmatch(
            r"REM(?:\s.*)?|CATALOG \d{13}|(?:TITLE|PERFORMER|SONGWRITER|ISRC) .+", line, re.I
        ):
            (metadata if current is None else current["metadata"]).append(line)
            continue
        raise ValueError("Unsupported CUE directive (not converted): " + line)
    if not 1 <= len(tracks) <= 99:
        raise ValueError("Expected 1 to 99 audio tracks")
    offset = 0
    for number, track in enumerate(tracks, 1):
        if track.get("number") != number:
            raise ValueError("Tracks must be numbered consecutively from 01")
        path = track["path"]
        with wave.open(str(path), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (
                2,
                2,
                44100,
                "NONE",
            ):
                raise ValueError("Requires uncompressed 44.1 kHz, 16-bit stereo WAV: " + path.name)
            frames = wav.getnframes()
        if not frames or frames % 588:
            raise ValueError("WAV must contain whole 588-sample CD sectors: " + path.name)
        count = frames // 588
        indexes = track["indexes"]
        if (
            [i for i, _ in indexes] not in ([1], [0, 1])
            or any(t >= count for _, t in indexes)
            or indexes != sorted(indexes, key=lambda p: p[1])
        ):
            raise ValueError("Invalid or unsupported track indexes: " + path.name)
        track.update(sectors=count, offset=offset)
        offset += count
    return tracks, metadata


def convert(source, output):
    source, output = Path(source).resolve(), Path(output).absolute()
    if output.exists():
        raise ValueError("Output directory already exists; choose a new directory")
    if any(c in output.name for c in '"\r\n'):
        raise ValueError("Output directory name cannot contain quotes or newlines")
    tracks, metadata = inspect(source)
    bin_name, cue_name = output.name + ".bin", output.name + ".cue"
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".audio-disc-", dir=output.parent))
    try:
        report = {
            "source_cue": str(source),
            "source_cue_sha1": digest(source),
            "format": "AUDIO/2352 BINARY (little-endian PCM)",
            "tracks": [],
            "hardware_verified": False,
        }
        lines = metadata + [f'FILE "{bin_name}" BINARY']
        with (staging / bin_name).open("wb") as target:
            for track in tracks:
                path = track["path"]
                pcm_hash = hashlib.sha1()
                written = 0
                before = digest(path)
                with wave.open(str(path), "rb") as wav:
                    while block := wav.readframes(588 * 256):
                        target.write(block)
                        pcm_hash.update(block)
                        written += len(block)
                if written != track["sectors"] * 2352 or digest(path) != before:
                    raise ValueError("WAV truncated or changed during conversion: " + str(path))
                lines.append(f"  TRACK {track['number']:02} AUDIO")
                lines.extend("    " + line for line in track["metadata"])
                for index, sector in track["indexes"]:
                    lines.append(f"    INDEX {index:02} {msf(track['offset'] + sector)}")
                report["tracks"].append(
                    {
                        "source": str(path),
                        "source_sha1": before,
                        "pcm_sha1": pcm_hash.hexdigest(),
                        "offset_sectors": track["offset"],
                        "sectors": track["sectors"],
                        "indexes": track["indexes"],
                    }
                )
        # Independently reread every output slice and compare its PCM digest.
        with (staging / bin_name).open("rb") as stream:
            for track in report["tracks"]:
                remaining = track["sectors"] * 2352
                check = hashlib.sha1()
                while remaining:
                    block = stream.read(min(1024 * 1024, remaining))
                    if not block:
                        raise ValueError("Incomplete output BIN")
                    check.update(block)
                    remaining -= len(block)
                if check.hexdigest() != track["pcm_sha1"]:
                    raise ValueError("Output audio verification failed")
            if stream.read(1):
                raise ValueError("Unexpected data after last audio track")
        (staging / cue_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
        if digest(source) != report["source_cue_sha1"]:
            raise ValueError("Source CUE changed during conversion")
        report["bin_sha1"] = digest(staging / bin_name)
        report["cue_sha1"] = digest(staging / cue_name)
        report["total_sectors"] = sum(t["sectors"] for t in tracks)
        report["duration_seconds"] = report["total_sectors"] / 75
        report["note"] = (
            "Original WAVs and recorded gaps preserved. ODE playback requires hardware testing."
        )
        (staging / "audio-disc.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
        # rename refuses to overwrite an existing nonempty output directory.
        if output.exists():
            raise ValueError("Output appeared during conversion")
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    print(
        f'Created {output / cue_name}: {len(tracks)} tracks, {report["duration_seconds"]:.2f} seconds; PCM verified'
    )
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_cue", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    try:
        convert(args.source_cue, args.output_directory)
    except (ValueError, OSError, wave.Error) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
