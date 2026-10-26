#!/usr/bin/env python3
"""Prepare a video preview with optional synchronized audio: streamed 320x240
(OM3DVID7, the default, as the desktop app makes them) or the older 160x120
clip that fits in RAM (OM3DVID5, --format 5).

Usage: prepare-preview.py SOURCE OUTPUT.3dv [--format 5|7] [--duration SECONDS]
                          [--start SECONDS] [--silent] [--budget-kib KIB]

FFmpeg decodes the source into raw frames and mono 8-bit sound, and a native
encoder library (built from preview7-encoder.c or preview-encoder.c by
build-preview-encoder.py) compresses them in the menu's own formats. The
picture is scaled to fit and centered on the menu's dark blue.
"""
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
V5_PACKET = 1200 * 12  # worst-case packet: every block GRAD with literal colors

AUDIO_RATE = 11025
MAX_START_SECONDS = 86400

# The menu's dark blue, shown around pictures that aren't 4:3.
BACKGROUND = "0x081021"


# OM3DVID7 (src/preview_v7.h): the whole clip up to a minute, streamed at
# about 200 KB/s. The same settings as the desktop app (desktop/src/media.rs).
STREAM_WIDTH, STREAM_HEIGHT = 320, 240
STREAM_MAX_SECONDS = 60
STREAM_MAX_FRAMES = STREAM_MAX_SECONDS * FPS
# A source that does not open from black gets this many frames fading in
# from black to its first frame. While they play, the encoder sends the
# first picture's dictionary entries, which one chunk cannot hold.
STREAM_LEAD_FRAMES = 12
STREAM_PARAMS = dict(
    rate=200 * 1024,
    read_rate=275 * 1024,
    slot_bytes=640 * 1024,
    max_chunk=160 * 1024,
    chunk_frames=10,
    entries4=8192,
    entries2=4096,
    cycle_cap=230000,
)


class StreamParams(ctypes.Structure):
    """STREAM_PARAMS as the C struct preview7-encoder.c expects."""

    _fields_ = [(name, ctypes.c_uint32) for name in STREAM_PARAMS]


def library_file_name(stem):
    if os.name == "nt":
        return stem + "-win64.dll"
    if sys.platform == "darwin":
        return stem + ".dylib"
    return stem + ".so"


def library_path(stem, directory=None):
    """A native library beside this tool, in build/media-native, or built now."""
    name = library_file_name(stem)
    folder = Path(__file__).resolve().parent
    if directory:
        candidates = [Path(directory) / name]
    else:
        candidates = [folder / name, folder.parent / "build/media-native" / name]

    path = next((p for p in candidates if p.is_file()), None)
    builder = folder / "build-preview-encoder.py"
    if not path and not directory and builder.is_file() and shutil.which("cc"):
        # Source checkout with a C compiler: build the libraries once.
        print("Building the preview encoder libraries...")
        subprocess.run([sys.executable, str(builder)], check=True, stdout=subprocess.DEVNULL)
        path = next((p for p in candidates if p.is_file()), None)
    return path


def stream_encoder(directory=None):
    """Load the streamed-preview encoder (tools/preview7-encoder.c), or None."""
    path = library_path("preview7-encoder", directory)
    if not path:
        return None
    library = ctypes.CDLL(str(path.resolve()))
    library.oh3do_preview7_abi.restype = ctypes.c_uint32
    if library.oh3do_preview7_abi() != 1:
        raise ValueError("Unsupported streamed preview encoder ABI: " + str(path))
    library.oh3do_preview7_workspace.restype = ctypes.c_uint64
    library.oh3do_preview7_workspace.argtypes = (ctypes.c_uint32,)
    library.oh3do_preview7_begin.argtypes = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32)
    library.oh3do_preview7_frame.argtypes = (ctypes.c_void_p, ctypes.c_char_p)
    library.oh3do_preview7_encode.restype = ctypes.c_int64
    library.oh3do_preview7_encode.argtypes = (
        ctypes.c_void_p,  # workspace
        ctypes.POINTER(StreamParams),
        ctypes.c_void_p,  # audio
        ctypes.c_uint64,  # audio samples
        ctypes.c_void_p,  # output
        ctypes.c_uint64,  # output capacity
        ctypes.POINTER(ctypes.c_uint32),  # statistics
    )
    return library


def encode_stream(library, frames, reserve, audio=None):
    """An OM3DVID7 file from 320x240 RGB24 frames (any iterable of frames, up
    to reserve of them) and signed 8-bit 11025 Hz sound for exactly those
    frames, or None. Returns (file bytes, frame count)."""
    workspace = ctypes.create_string_buffer(library.oh3do_preview7_workspace(reserve))
    if library.oh3do_preview7_begin(workspace, len(workspace), reserve):
        raise ValueError("Preview encoder setup failed")

    count = 0
    for frame in frames:
        if count == reserve or library.oh3do_preview7_frame(workspace, frame):
            raise ValueError("Too many or invalid preview frames")
        count += 1
    if not count:
        raise ValueError("Video did not produce complete frames")

    samples = count * AUDIO_RATE // FPS
    if audio is not None and len(audio) != samples:
        raise ValueError("Preview sound does not match its frames")

    # Room for the target data rate plus 10%, and 2 MiB for headers.
    capacity = STREAM_PARAMS["rate"] * count // FPS * 11 // 10 + (2 << 20)
    output = ctypes.create_string_buffer(capacity)
    stats = (ctypes.c_uint32 * 8)()
    length = library.oh3do_preview7_encode(
        workspace,
        ctypes.byref(StreamParams(**STREAM_PARAMS)),
        audio,
        samples if audio else 0,
        output,
        capacity,
        stats,
    )
    if length < 0:
        raise ValueError(f"Preview encoder failed (error {length})")
    return output.raw[:length], count


# A first frame darker than this mean value (of 255) already opens from black.
DARK_FRAME = 6


def fade_in(frame):
    """STREAM_LEAD_FRAMES RGB24 frames brightening from black to frame."""
    frames = []
    for step in range(STREAM_LEAD_FRAMES):
        # Scale every channel value by step / STREAM_LEAD_FRAMES.
        table = bytes(value * step // STREAM_LEAD_FRAMES for value in range(256))
        frames.append(frame.translate(table))
    return frames


# Steps shared by both formats


def check_request(source, target, start):
    if (
        not source.is_file()
        or target.suffix.lower() != ".3dv"
        or source.resolve() == target.resolve()
        or not math.isfinite(start)
        or not 0 <= start <= MAX_START_SECONDS
    ):
        raise ValueError("Choose an existing source, a separate .3dv output and a valid start time")


def find_ffmpeg():
    """The paths of ffmpeg and ffprobe, which must both be installed."""
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise ValueError("FFmpeg and ffprobe are required on PATH")
    return ffmpeg, ffprobe


def probe(ffprobe, source):
    """Ask ffprobe about the source's streams and format.

    Returns (the probe's answer, whether the source has sound). Raises
    ValueError if it has no video.
    """
    command = [ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(source)]
    info = json.loads(subprocess.run(command, check=True, stdout=subprocess.PIPE).stdout)
    kinds = [stream.get("codec_type") for stream in info.get("streams", [])]
    if "video" not in kinds:
        raise ValueError("Source has no video stream")
    return info, "audio" in kinds


def requested_seconds(duration, default):
    """The requested duration in seconds; None or "full" means `default`."""
    if duration is None or str(duration).lower() == "full":
        wanted = default
    else:
        wanted = float(duration)
    if not math.isfinite(wanted) or wanted <= 0:
        raise ValueError("Preview duration must be a finite value greater than zero")
    return wanted


def seconds_after(info, start):
    """How much of the source follows `start`, or NaN when unknown."""
    try:
        return float(info.get("format", {}).get("duration", "nan")) - start
    except (ValueError, TypeError):
        return float("nan")


def video_command(ffmpeg, source, start, seconds, width, height, frame_limit):
    """FFmpeg arguments that write the first video stream as raw RGB24 frames
    to standard output: resampled to 30 fps, scaled to fit width x height and
    centered on the background color."""
    scale = f"scale={width}:{height}:force_original_aspect_ratio=decrease:flags=area"
    pad = f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:color={BACKGROUND}"
    return [
        ffmpeg,
        "-v",
        "error",
        "-nostdin",
        "-ss",
        str(start),
        "-i",
        str(source),
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-t",
        str(seconds),
        "-vf",
        f"fps={FPS},{scale},{pad}",
        "-frames:v",
        str(frame_limit),
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "pipe:1",
    ]


def read_audio(ffmpeg, source, start, seconds):
    """The first audio stream as signed 8-bit mono at 11025 Hz, slightly
    quieter and with 10 ms fades at each end to avoid clicks."""
    fades = f"afade=t=in:d=0.01,afade=t=out:st={max(0, seconds - 0.01)}:d=0.01"
    command = [
        ffmpeg,
        "-v",
        "error",
        "-nostdin",
        "-ss",
        str(start),
        "-i",
        str(source),
        "-map",
        "0:a:0",
        "-vn",
        "-sn",
        "-t",
        str(seconds),
        "-ac",
        "1",
        "-ar",
        str(AUDIO_RATE),
        "-af",
        f"volume=0.8,{fades}",
        "-f",
        "s8",
        "pipe:1",
    ]
    return subprocess.run(command, check=True, stdout=subprocess.PIPE).stdout


def write_replacing(target, data):
    """Write through a temporary file, so a reader never sees half of it."""
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".tmp")
    try:
        temporary.write_bytes(data)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)


# OM3DVID7


def convert_stream(source, target, start=0, duration=None, sound=True, encoder=None):
    """OM3DVID7: 320x240 at 30 fps, the whole source up to a minute, streamed.

    Returns a summary: seconds, frames, fps, bytes and audio_bytes.
    """
    source, target = Path(source), Path(target)
    check_request(source, target, start)
    library = encoder or stream_encoder()
    if not library:
        raise ValueError("The preview encoder library is missing; run build-preview-encoder.py")
    ffmpeg, ffprobe = find_ffmpeg()
    info, has_sound = probe(ffprobe, source)
    sound = sound and has_sound

    wanted = requested_seconds(duration, STREAM_MAX_SECONDS)
    available = seconds_after(info, start)
    if math.isfinite(available):
        seconds = min(wanted, STREAM_MAX_SECONDS, available)
    else:
        seconds = min(wanted, STREAM_MAX_SECONDS)
    if seconds <= 0:
        raise ValueError("Start position is beyond the end of the source")

    # Two frames of slack for rounding in FFmpeg's frame timing.
    reserve = min(STREAM_MAX_FRAMES, math.ceil(seconds * FPS) + 2)
    frame_size = STREAM_WIDTH * STREAM_HEIGHT * 3
    command = video_command(ffmpeg, source, start, seconds, STREAM_WIDTH, STREAM_HEIGHT, reserve)
    reader = subprocess.Popen(command, stdout=subprocess.PIPE)
    frames = iter(lambda: reader.stdout.read(frame_size), b"")
    try:
        kept = [frame for frame in frames if len(frame) == frame_size]

        # Fade in from black unless the clip already starts dark, keeping the
        # total within the minute.
        starts_bright = kept and sum(kept[0]) >= DARK_FRAME * frame_size
        lead = fade_in(kept[0]) if starts_bright else []
        kept = kept[: STREAM_MAX_FRAMES - len(lead)]
        count = len(kept)

        audio = None
        if sound:
            samples = count * AUDIO_RATE // FPS
            audio = read_audio(ffmpeg, source, start, count / FPS)
            audio = audio[:samples].ljust(samples, b"\0")
            # Silence under the fade-in frames.
            total = (count + len(lead)) * AUDIO_RATE // FPS
            audio = bytes(total - len(audio)) + audio

        data, count = encode_stream(
            library, lead + kept, min(STREAM_MAX_FRAMES, reserve + len(lead)), audio
        )
    finally:
        reader.stdout.close()
        if reader.wait():
            raise ValueError("FFmpeg failed to read the source video")

    write_replacing(target, data)
    print(
        f"Created {target}: {count/FPS:.2f} seconds, {count} frames at {FPS} fps, "
        f"{len(data)/1024:.1f} KiB streamed" + ("; 11025 Hz mono audio" if audio else "; silent")
    )
    return dict(
        seconds=count / FPS, frames=count, fps=FPS, bytes=len(data), audio_bytes=len(audio or b"")
    )


# OM3DVID5


def native_encoder(directory=None):
    """Load the OM3DVID5 encoder library beside this tool (or an explicit test dir)."""
    path = library_path("preview-encoder", directory)
    if not path:
        return None
    library = ctypes.CDLL(str(path.resolve()))
    library.oh3do_preview_abi.restype = ctypes.c_uint
    if library.oh3do_preview_abi() != 2:
        raise ValueError("Unsupported preview encoder ABI: " + str(path))
    encode = library.oh3do_preview_encode
    encode.argtypes = (
        ctypes.c_char_p,  # frames
        ctypes.c_uint,  # frame count
        ctypes.c_double,  # lambda for the first `split` frames
        ctypes.c_uint,  # split
        ctypes.c_double,  # lambda for the rest
        ctypes.c_void_p,  # output
        ctypes.c_ulong,  # output capacity
        ctypes.c_void_p,  # decoded frames, or NULL
    )
    encode.restype = ctypes.c_long
    return encode


def encode_v5(frames, count, budget, encode, decoded=None):
    """Smallest lambda (best quality) whose video section fits budget bytes.

    Size jumps between nearby lambdas, so the rest of the budget is filled by
    encoding the first frames with the next smaller lambda.

    Lambda trades quality for size: the encoder picks each block's coding to
    minimize distortion + lambda * bits, so a larger lambda gives a smaller,
    blurrier clip. `decoded`, if given, receives the frames as the console
    will show them.
    """
    output = ctypes.create_string_buffer((count + 1) * 4 + count * V5_PACKET)

    def encoded_size(first_lambda, split, rest_lambda, keep=None):
        length = encode(frames, count, first_lambda, split, rest_lambda, output, len(output), keep)
        if length < 0:
            raise ValueError(f"Preview encoder failed (error {length})")
        return length

    def fits(value):
        return encoded_size(value, 0, value) <= budget

    # Bracket from a typical lambda, widening by 4x, then bisect to 2%.
    # Afterwards `high` fits the budget and `low` doesn't (or low == high
    # when even the best quality fits).
    low, high = 0.5, 0.5
    if fits(high):
        while low > 0.01:
            low = max(0.01, low / 4)
            if not fits(low):
                break
            high = low
        else:
            low = high  # best quality fits
    else:
        while True:
            low, high = high, high * 4
            if high > 100000:
                raise ValueError(
                    "Preview video cannot fit the RAM budget; choose a shorter duration"
                )
            if fits(high):
                break
    while high / low > 1.02:
        middle = math.sqrt(low * high)
        if fits(middle):
            high = middle
        else:
            low = middle

    # Use the better-quality `low` for as many leading frames as still fit.
    split = 0
    if low < high:
        fitting, too_many = 0, count + 1
        while too_many - fitting > 1:
            middle = (fitting + too_many) // 2
            if encoded_size(low, middle, high) <= budget:
                fitting = middle
            else:
                too_many = middle
        split = fitting

    length = encoded_size(low, split, high, decoded)
    return output.raw[:length]


def word_sum(data):
    """OM3DVID5 payload check (see src/usb_media.c): over big-endian words,
    a += w; b += a; result a ^ (b rotated by 16), modulo 2**32."""
    a = b = 0
    for (word,) in struct.iter_unpack(">I", data):
        a = (a + word) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    b_rotated = ((b << 16) | (b >> 16)) & 0xFFFFFFFF
    return a ^ b_rotated


def audio_bytes(count):
    """Signed 8-bit 11025 Hz samples for count frames, padded to a word."""
    samples = (count * AUDIO_RATE + FPS - 1) // FPS  # rounded up
    return (samples + 3) & ~3


def v5_container(video, count, audio):
    """Frame table, audio, video packets, zero padding to a word boundary.
    The menu reads the audio first, then splits the packets across its memory.

    Header after "OM3DVID5" (big-endian): width, height (16 bits), payload
    size, payload word_sum (32), frame count, fps (16), video size, audio
    size, audio rate and audio size again (32). The file ends "END3DV05".
    """
    table = (count + 1) * 4
    payload = video[:table] + audio + video[table:] + bytes(-len(video) % 4)
    header = struct.pack(
        ">HHIIHHIIII",
        WIDTH,
        HEIGHT,
        len(payload),
        word_sum(payload),
        count,
        FPS,
        len(video),
        len(audio),
        AUDIO_RATE if audio else 0,
        len(audio),
    )
    return b"OM3DVID5" + header + payload + b"END3DV05"


def convert(source, target, start=0, duration=None, sound=True, budget=V5_BYTES, encoder=None):
    """OM3DVID5: block codec, 160x120 at 30 fps, up to 15 s, sized to budget bytes.

    Returns a summary: seconds, frames, fps, bytes (the payload the menu
    loads into memory) and audio_bytes.
    """
    source, target = Path(source), Path(target)
    check_request(source, target, start)
    encode = encoder or native_encoder()
    if not encode:
        raise ValueError("The preview encoder library is missing; run build-preview-encoder.py")
    ffmpeg, ffprobe = find_ffmpeg()
    info, has_sound = probe(ffprobe, source)
    sound = sound and has_sound

    wanted = requested_seconds(duration, CLIP_CAP_SECONDS)
    if wanted > CLIP_CAP_SECONDS:
        print("Preview length capped at 15 seconds.")
    seconds = min(wanted, CLIP_CAP_SECONDS)
    available = seconds_after(info, start)
    if math.isfinite(available):
        seconds = min(seconds, available)
    if seconds <= 0:
        raise ValueError("Start position is beyond the end of the source")

    limit = min(V5_FRAMES, math.ceil(seconds * FPS))
    command = video_command(ffmpeg, source, start, seconds, WIDTH, HEIGHT, limit)
    raw = subprocess.run(command, check=True, stdout=subprocess.PIPE).stdout
    frame_size = WIDTH * HEIGHT * 3
    if not raw or len(raw) % frame_size or len(raw) > limit * frame_size:
        raise ValueError("Video did not produce complete bounded frames")
    count = len(raw) // frame_size

    # The video gets whatever the sound leaves of the budget, but at least
    # the frame table and a minimal packet per frame.
    audio_size = audio_bytes(count) if sound else 0
    if budget - audio_size < (count + 1) * 4 + count * 8:
        raise ValueError("Preview RAM budget is too small for this clip")
    video = encode_v5(raw, count, budget - audio_size, encode)
    seconds = count / FPS

    audio = b""
    if sound:
        audio = read_audio(ffmpeg, source, start, seconds)[:audio_size].ljust(audio_size, b"\0")

    data = v5_container(video, count, audio)
    payload_bytes = len(data) - 48  # 40-byte header, 8-byte end marker
    write_replacing(target, data)
    print(
        f"Created {target}: {seconds:.2f} seconds, {count} frames at {FPS} fps, "
        f"{payload_bytes/1024:.1f} KiB RAM" + ("; 11025 Hz mono audio" if audio else "; silent")
    )
    return dict(seconds=seconds, frames=count, fps=FPS, bytes=payload_bytes, audio_bytes=len(audio))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="the game's .ohmy3do/NAME.3dv")
    parser.add_argument(
        "--format",
        type=int,
        choices=(5, 7),
        default=7,
        help="7: streamed 320x240, the whole clip up to a minute (default); "
        "5: 160x120 in RAM, up to 15 s",
    )
    parser.add_argument(
        "--duration", default=None, help="maximum seconds (default: full, up to 60; format 5: 15)"
    )
    parser.add_argument("--silent", action="store_true", help="omit audio")
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument(
        "--budget-kib",
        type=int,
        default=V5_BYTES // 1024,
        help="format 5: RAM for video and audio together (default: %(default)s)",
    )
    args = parser.parse_args()
    try:
        if args.format == 7:
            convert_stream(args.source, args.output, args.start, args.duration, not args.silent)
            return
        if not 64 <= args.budget_kib <= 4096:
            raise ValueError("Choose a budget of 64-4096 KiB")
        convert(
            args.source,
            args.output,
            args.start,
            args.duration,
            not args.silent,
            args.budget_kib * 1024,
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
