#!/usr/bin/env python3
"""Losslessly combine a CD-quality WAV/CUE audio disc into one BIN/CUE pair.

Usage: prepare-audio-disc.py SOURCE.cue OUTPUT_FOLDER

Original files are never modified. The output directory must not already exist.
Track indexes (including recorded pregaps) are preserved, without resampling.
Console playback depends on the installed ODE firmware.

The output folder holds <name>.bin (every track's audio, back to back),
<name>.cue (the track list pointing into it) and audio-disc.json (a report
with SHA-1 hashes of the sources and output). The folder is built under a
temporary name and renamed only once every check has passed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import wave

# A CD sector holds 1/75 of a second: 588 stereo 16-bit samples, 2352 bytes.
SECTORS_PER_SECOND = 75
SAMPLES_PER_SECTOR = 588
BYTES_PER_SECTOR = 2352

# The only WAV format a CD can hold: (channels, bytes per sample, rate, compression).
CD_AUDIO_FORMAT = (2, 2, 44100, "NONE")

MAX_TRACKS = 99
READ_BLOCK = 1024 * 1024
WAV_READ_SECTORS = 256

# CUE lines that carry descriptions rather than layout, copied unchanged.
METADATA_LINE = r"REM(?:\s.*)?|CATALOG \d{13}|(?:TITLE|PERFORMER|SONGWRITER|ISRC) .+"


def digest(path):
    """SHA-1 of a file, read a block at a time."""
    with path.open("rb") as stream:
        result = hashlib.sha1()
        for block in iter(lambda: stream.read(READ_BLOCK), b""):
            result.update(block)
        return result.hexdigest()


def sectors(value):
    """Convert a CUE time "MM:SS:FF" (FF = 1/75 s) to a sector count."""
    minute, second, frame = map(int, value.split(":"))
    if second >= 60 or frame >= SECTORS_PER_SECOND:
        raise ValueError("Invalid CD time: " + value)
    return (minute * 60 + second) * SECTORS_PER_SECOND + frame


def msf(value):
    """Convert a sector count to a CUE time "MM:SS:FF"."""
    minute, rest = divmod(value, 60 * SECTORS_PER_SECOND)
    second, frame = divmod(rest, SECTORS_PER_SECOND)
    return f"{minute:02}:{second:02}:{frame:02}"


def parse_cue(source):
    """Read a CUE sheet with one WAV file per audio track.

    Returns (tracks, disc metadata lines). Each track is a dict with "path"
    (its WAV), "number", "indexes" ([(index number, sector within the
    track)]) and "metadata" (its descriptive lines). Raises ValueError for
    anything else the sheet contains, so nothing is silently dropped.
    """
    tracks = []
    current = None
    metadata = []
    for line in source.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line:
            continue

        # FILE "name.wav" WAVE starts a new track's file.
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

        # Descriptions before the first FILE belong to the disc; later ones
        # to the current track.
        if re.fullmatch(METADATA_LINE, line, re.I):
            if current is None:
                metadata.append(line)
            else:
                current["metadata"].append(line)
            continue

        raise ValueError("Unsupported CUE directive (not converted): " + line)
    return tracks, metadata


def wav_sector_count(path):
    """Return how many CD sectors a WAV holds, checking it is CD audio made
    of whole sectors."""
    with wave.open(str(path), "rb") as wav:
        audio_format = (
            wav.getnchannels(),
            wav.getsampwidth(),
            wav.getframerate(),
            wav.getcomptype(),
        )
        if audio_format != CD_AUDIO_FORMAT:
            raise ValueError("Requires uncompressed 44.1 kHz, 16-bit stereo WAV: " + path.name)
        frames = wav.getnframes()
    if not frames or frames % SAMPLES_PER_SECTOR:
        raise ValueError("WAV must contain whole 588-sample CD sectors: " + path.name)
    return frames // SAMPLES_PER_SECTOR


def check_indexes(indexes, count, path):
    """A track may have INDEX 01 (its start), optionally after INDEX 00 (a
    pregap), each inside the track and in order."""
    numbers = [number for number, _ in indexes]
    in_track = all(sector < count for _, sector in indexes)
    in_order = indexes == sorted(indexes, key=lambda pair: pair[1])
    if numbers not in ([1], [0, 1]) or not in_track or not in_order:
        raise ValueError("Invalid or unsupported track indexes: " + path.name)


def inspect(source):
    """Parse and check a WAV/CUE disc before anything is written.

    Returns (tracks, disc metadata) as parse_cue does, with each track also
    given "sectors" (its length) and "offset" (where it starts in the BIN).
    """
    tracks, metadata = parse_cue(source)
    if not 1 <= len(tracks) <= MAX_TRACKS:
        raise ValueError("Expected 1 to 99 audio tracks")

    offset = 0
    for number, track in enumerate(tracks, 1):
        if track.get("number") != number:
            raise ValueError("Tracks must be numbered consecutively from 01")
        count = wav_sector_count(track["path"])
        check_indexes(track["indexes"], count, track["path"])
        track.update(sectors=count, offset=offset)
        offset += count
    return tracks, metadata


def copy_track_audio(track, target):
    """Append a track's PCM samples to the open BIN file `target`.

    Returns (the WAV's SHA-1, the PCM data's SHA-1). Raises ValueError if the
    WAV is shorter than inspected or changes while it is read.
    """
    path = track["path"]
    pcm_hash = hashlib.sha1()
    written = 0
    before = digest(path)
    with wave.open(str(path), "rb") as wav:
        while block := wav.readframes(SAMPLES_PER_SECTOR * WAV_READ_SECTORS):
            target.write(block)
            pcm_hash.update(block)
            written += len(block)
    if written != track["sectors"] * BYTES_PER_SECTOR or digest(path) != before:
        raise ValueError("WAV truncated or changed during conversion: " + str(path))
    return before, pcm_hash.hexdigest()


def cue_track_lines(track):
    """The output CUE's lines for one track, with its indexes moved to the
    track's position in the combined BIN."""
    lines = [f"  TRACK {track['number']:02} AUDIO"]
    lines.extend("    " + line for line in track["metadata"])
    for index, sector in track["indexes"]:
        lines.append(f"    INDEX {index:02} {msf(track['offset'] + sector)}")
    return lines


def verify_bin(path, report_tracks):
    """Independently reread every output slice and compare its PCM digest,
    and check nothing follows the last track."""
    with path.open("rb") as stream:
        for track in report_tracks:
            remaining = track["sectors"] * BYTES_PER_SECTOR
            check = hashlib.sha1()
            while remaining:
                block = stream.read(min(READ_BLOCK, remaining))
                if not block:
                    raise ValueError("Incomplete output BIN")
                check.update(block)
                remaining -= len(block)
            if check.hexdigest() != track["pcm_sha1"]:
                raise ValueError("Output audio verification failed")
        if stream.read(1):
            raise ValueError("Unexpected data after last audio track")


def write_disc(source, tracks, metadata, staging, bin_name, cue_name):
    """Write the BIN, CUE and report into `staging` and return the report."""
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
            source_sha1, pcm_sha1 = copy_track_audio(track, target)
            lines.extend(cue_track_lines(track))
            report["tracks"].append(
                {
                    "source": str(track["path"]),
                    "source_sha1": source_sha1,
                    "pcm_sha1": pcm_sha1,
                    "offset_sectors": track["offset"],
                    "sectors": track["sectors"],
                    "indexes": track["indexes"],
                }
            )

    verify_bin(staging / bin_name, report["tracks"])
    (staging / cue_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    if digest(source) != report["source_cue_sha1"]:
        raise ValueError("Source CUE changed during conversion")

    report["bin_sha1"] = digest(staging / bin_name)
    report["cue_sha1"] = digest(staging / cue_name)
    report["total_sectors"] = sum(track["sectors"] for track in tracks)
    report["duration_seconds"] = report["total_sectors"] / SECTORS_PER_SECOND
    report["note"] = (
        "Original WAVs and recorded gaps preserved. ODE playback requires hardware testing."
    )
    (staging / "audio-disc.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def convert(source, output):
    """Convert the disc described by the CUE `source` into the new folder
    `output`, named after it. Returns the report written to audio-disc.json."""
    source = Path(source).resolve()
    output = Path(output).absolute()
    if output.exists():
        raise ValueError("Output directory already exists; choose a new directory")
    if any(c in output.name for c in '"\r\n'):
        raise ValueError("Output directory name cannot contain quotes or newlines")

    tracks, metadata = inspect(source)
    bin_name = output.name + ".bin"
    cue_name = output.name + ".cue"

    # Build in a hidden folder beside the output, so the final rename is
    # quick and a failure leaves nothing under the output's name.
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".audio-disc-", dir=output.parent))
    try:
        report = write_disc(source, tracks, metadata, staging, bin_name, cue_name)
        # rename refuses to overwrite an existing nonempty output directory.
        if output.exists():
            raise ValueError("Output appeared during conversion")
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)

    print(
        f"Created {output / cue_name}: {len(tracks)} tracks, "
        f'{report["duration_seconds"]:.2f} seconds; PCM verified'
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
