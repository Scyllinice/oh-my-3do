"""OM3DVID7 for Python: make streamed 3DO video clips, and check them.

OM3DVID7 is the 320x240, 30 fps streamed video format of the om3dvid
library (see om3dvid/include/om3dvid7.h for the layout). This module needs
only the standard library.

Encoding uses the library's native encoder, built from om3dvid/src as a
shared library (om3dvid7.so, om3dvid7.dylib or om3dvid7-win64.dll; see
tools/build-preview-encoder.py). Reading headers and checking files is pure
Python.

    import om3dvid7
    encoder = om3dvid7.Encoder(max_frames=len(frames))
    for frame in frames:              # 320x240 RGB, 3 bytes a pixel, in rows
        encoder.add_frame(frame)
    clip = encoder.finish(audio)      # audio: signed 8-bit mono at 11025 Hz,
                                      # om3dvid7.samples_for(frames) of them,
                                      # or None for a silent clip
    open("clip.3dv", "wb").write(clip.data)
    om3dvid7.check(clip.data)         # raises ValueError if a player would refuse it

Sound is optional throughout: a clip may be silent, and a player decides for
itself whether and how to play the sound it carries.
"""

import ctypes
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

WIDTH, HEIGHT, FPS = 320, 240, 30
AUDIO_RATE = 11025
MAX_FRAMES = 1800
MAX_ENTRIES4, MAX_ENTRIES2 = 8192, 4096
MAX_CHUNK_FRAMES = 30

# The bytes of one input frame: 320x240 pixels of 8-bit RGB.
FRAME_BYTES = WIDTH * HEIGHT * 3

# The file's layout (om3dvid7.h): a 2048-byte header block, whose chunk
# table starts at byte 64, then chunks padded to 2048 bytes, each starting
# with a 24-byte header. A 4x4 dictionary entry is 40 bytes, a 2x2 one 12.
BLOCK_BYTES = 2048
CHUNK_TABLE = 64
CHUNK_HEADER = 24
ENTRY4_BYTES, ENTRY2_BYTES = 40, 12

# The native encoder's interface version this module speaks
# (OM3DVID7_ENCODER_ABI in om3dvid7_encoder.h).
ENCODER_ABI = 2

# What om3dvid7_encoder_finish reports, in its order.
STATISTICS = (
    "lambda_x1000",
    "slowest_frame_clocks",
    "entries4",
    "entries2",
    "preroll",
    "slots",
    "largest_chunk",
    "chunks",
)

# om3dvid7_encoder_finish's errors.
FINISH_ERRORS = {
    -1: "bad arguments",
    -2: "the output buffer is too small",
    -3: "decoding the result disagreed with the encoder",
    -4: "no quality setting meets both the data rate and the reader model",
}


def samples_for(frames):
    """The sound samples a clip of `frames` frames carries (rounded down)."""
    return frames * AUDIO_RATE // FPS


def word_sum(data):
    """The format's check value of `data` (a whole number of 32-bit
    big-endian words), or None if it is not whole words."""
    a = b = 0
    if len(data) % 4:
        return None
    for (word,) in struct.iter_unpack(">I", data):
        a = (a + word) & 0xFFFFFFFF
        b = (b + a) & 0xFFFFFFFF
    return a ^ (((b << 16) | (b >> 16)) & 0xFFFFFFFF)


# ---- The native encoder --------------------------------------------------


class EncoderSettings(ctypes.Structure):
    """What the encoder aims for (Om3dvid7EncoderSettings): all 32-bit."""

    _fields_ = [
        ("rate", ctypes.c_uint32),  # bytes per second the file should average
        ("read_rate", ctypes.c_uint32),  # bytes per second the console's reader manages
        ("slot_bytes", ctypes.c_uint32),  # memory the player is assumed to have for chunks
        ("max_chunk", ctypes.c_uint32),  # largest padded chunk, a multiple of 2048
        ("chunk_frames", ctypes.c_uint32),
        ("entries4", ctypes.c_uint32),
        ("entries2", ctypes.c_uint32),
        ("cycle_cap", ctypes.c_uint32),  # modelled ARM60 decode clocks per frame
    ]

    def as_dict(self):
        return {name: getattr(self, name) for name, _ in self._fields_}


def library_file_name():
    """The native library's file name on this platform."""
    if os.name == "nt":
        return "om3dvid7-win64.dll"
    if sys.platform == "darwin":
        return "om3dvid7.dylib"
    return "om3dvid7.so"


class Library:
    """The loaded native library, with its functions' types declared."""

    def __init__(self, path):
        self.path = Path(path)
        native = ctypes.CDLL(str(self.path.resolve()))
        native.om3dvid7_encoder_abi.restype = ctypes.c_uint32
        if native.om3dvid7_encoder_abi() != ENCODER_ABI:
            raise ValueError("Unsupported OM3DVID7 encoder ABI: " + str(self.path))
        native.om3dvid7_encoder_default_settings.restype = None
        native.om3dvid7_encoder_default_settings.argtypes = (ctypes.POINTER(EncoderSettings),)
        native.om3dvid7_encoder_workspace_bytes.restype = ctypes.c_uint64
        native.om3dvid7_encoder_workspace_bytes.argtypes = (ctypes.c_uint32,)
        native.om3dvid7_encoder_begin.restype = ctypes.c_int32
        native.om3dvid7_encoder_begin.argtypes = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32)
        native.om3dvid7_encoder_add_frame.restype = ctypes.c_int32
        native.om3dvid7_encoder_add_frame.argtypes = (ctypes.c_void_p, ctypes.c_char_p)
        native.om3dvid7_encoder_finish.restype = ctypes.c_int64
        native.om3dvid7_encoder_finish.argtypes = (
            ctypes.c_void_p,  # workspace
            ctypes.POINTER(EncoderSettings),
            ctypes.c_void_p,  # audio, or None
            ctypes.c_uint64,  # audio samples
            ctypes.c_void_p,  # output
            ctypes.c_uint64,  # output capacity
            ctypes.POINTER(ctypes.c_uint32),  # statistics
        )
        self.native = native

    def default_settings(self):
        """The settings Oh My 3DO uses (about 200 KB/s, 10-frame chunks)."""
        settings = EncoderSettings()
        self.native.om3dvid7_encoder_default_settings(ctypes.byref(settings))
        return settings


_loaded = {}


def load_library(path=None):
    """Load the native library: `path` (the file, or a folder holding it),
    else $OM3DVID7_LIBRARY, else the file beside this module. Each file is
    loaded once. Raises FileNotFoundError if there is none."""
    if path is None:
        path = os.environ.get("OM3DVID7_LIBRARY") or Path(__file__).resolve().parent
    path = Path(path)
    if path.is_dir():
        path = path / library_file_name()
    if not path.is_file():
        raise FileNotFoundError("OM3DVID7 native library not found: " + str(path))
    key = str(path.resolve())
    if key not in _loaded:
        _loaded[key] = Library(path)
    return _loaded[key]


@dataclass
class EncodedClip:
    """An encoded clip: the file's bytes, its frame count, and the encoder's
    statistics (named as in STATISTICS)."""

    data: bytes
    frames: int
    statistics: dict


class Encoder:
    """Encodes one clip of up to `max_frames` frames.

    `library` is a Library (from load_library), or a path for load_library,
    or None to find it."""

    def __init__(self, max_frames, library=None):
        if not isinstance(library, Library):
            library = load_library(library)
        self.library = library
        native = library.native
        self.max_frames = max_frames
        size = native.om3dvid7_encoder_workspace_bytes(max_frames)
        if not size:
            raise ValueError(f"A clip holds 1 to {MAX_FRAMES} frames, not {max_frames}")
        self.workspace = ctypes.create_string_buffer(size)
        if native.om3dvid7_encoder_begin(self.workspace, size, max_frames):
            raise ValueError("Preview encoder setup failed")
        self.frames = 0

    def add_frame(self, rgb):
        """Adds the next frame: FRAME_BYTES of 320x240 RGB, in rows."""
        if len(rgb) != FRAME_BYTES:
            raise ValueError(f"A frame is {FRAME_BYTES} bytes of 320x240 RGB, not {len(rgb)}")
        if self.frames == self.max_frames or self.library.native.om3dvid7_encoder_add_frame(
            self.workspace, bytes(rgb)
        ):
            raise ValueError("Too many or invalid preview frames")
        self.frames += 1

    def finish(self, audio=None, settings=None):
        """Encodes the frames added. `audio` is signed 8-bit mono at 11025 Hz,
        exactly samples_for(frames) bytes, or None for a silent clip.
        `settings` defaults to the library's. Returns an EncodedClip."""
        if not self.frames:
            raise ValueError("Video did not produce complete frames")
        samples = samples_for(self.frames)
        if audio is not None and len(audio) != samples:
            raise ValueError("Preview sound does not match its frames")
        if settings is None:
            settings = self.library.default_settings()

        # Room for the target data rate plus 10%, and 2 MiB for headers.
        capacity = settings.rate * self.frames // FPS * 11 // 10 + (2 << 20)
        output = ctypes.create_string_buffer(capacity)
        statistics = (ctypes.c_uint32 * len(STATISTICS))()
        length = self.library.native.om3dvid7_encoder_finish(
            self.workspace,
            ctypes.byref(settings),
            bytes(audio) if audio else None,
            samples if audio else 0,
            output,
            capacity,
            statistics,
        )
        if length < 0:
            reason = FINISH_ERRORS.get(length, "unknown error")
            raise ValueError(f"Preview encoder failed (error {length}: {reason})")
        return EncodedClip(output.raw[:length], self.frames, dict(zip(STATISTICS, statistics)))


def encode(frames, audio=None, settings=None, library=None, max_frames=None):
    """Encodes `frames` (an iterable of 320x240 RGB frames; at most
    `max_frames` of them, default its length) with optional `audio` in one
    call. Returns an EncodedClip."""
    if max_frames is None:
        frames = list(frames)
        max_frames = len(frames)
    encoder = Encoder(max_frames, library)
    for frame in frames:
        encoder.add_frame(frame)
    return encoder.finish(audio, settings)


# ---- Reading and checking files -------------------------------------------


@dataclass
class Header:
    """A clip's header block (see om3dvid7.h)."""

    frames: int
    chunk_frames: int  # frames per chunk (the last may have fewer)
    chunks: int
    preroll: int  # chunks a player reads before it starts playing
    entries4: int  # dictionary entries in the whole clip
    entries2: int
    audio_rate: int  # 0 (silent) or 11025
    largest_chunk: int  # bytes
    rate: int  # nominal bytes per second
    slots: int  # chunk buffers the encoder assumed the player has
    chunk_blocks: tuple  # each chunk's length in 2048-byte blocks

    @property
    def seconds(self):
        return self.frames / FPS

    @property
    def has_sound(self):
        return self.audio_rate != 0


def read_header(data):
    """Reads and checks the header block at the start of `data` (at least
    2048 bytes), as a player does. Returns a Header, or raises ValueError."""
    if len(data) < BLOCK_BYTES:
        raise ValueError("Truncated streamed preview")
    if data[:8] != b"OM3DVID7":
        raise ValueError("Not an OM3DVID7 clip")
    (
        width,
        height,
        fps,
        chunk_frames,
        frames,
        chunks,
        preroll,
        entries4,
        entries2,
        audio_rate,
        largest_chunk,
        rate,
        header_sum,
        slots,
    ) = struct.unpack_from(">HHHHIHHHHIIIIH", data, 8)
    max_chunks = (BLOCK_BYTES - CHUNK_TABLE) // 2
    table_end = CHUNK_TABLE + 2 * chunks
    if (
        (width, height, fps) != (WIDTH, HEIGHT, FPS)
        or not 1 <= chunk_frames <= MAX_CHUNK_FRAMES
        or not 1 <= frames <= MAX_FRAMES
        or chunks != -(-frames // chunk_frames)  # frames / per chunk, rounded up
        or chunks > max_chunks
        or not 1 <= preroll <= chunks
        or slots < preroll
        or entries4 > MAX_ENTRIES4
        or entries2 > MAX_ENTRIES2
        or audio_rate not in (0, AUDIO_RATE)
        or not largest_chunk
        or largest_chunk % BLOCK_BYTES
        or any(data[46:CHUNK_TABLE])
        or any(data[table_end:BLOCK_BYTES])
    ):
        raise ValueError("Invalid streamed preview header")
    # The sum covers the header block with its own field (bytes 40-43) as zero.
    if word_sum(bytes(data[:40]) + bytes(4) + bytes(data[44:BLOCK_BYTES])) != header_sum:
        raise ValueError("Streamed preview header checksum mismatch")
    blocks = tuple(count for (count,) in struct.iter_unpack(">H", data[CHUNK_TABLE:table_end]))
    return Header(
        frames,
        chunk_frames,
        chunks,
        preroll,
        entries4,
        entries2,
        audio_rate,
        largest_chunk,
        rate,
        slots,
        blocks,
    )


def check(data):
    """Checks a whole clip as a player does: the header block and its sum,
    the chunk table against the file, and every chunk's sum, numbering,
    entries and frame table. (Packets are checked by the decoder as they
    play.) Returns the Header, or raises ValueError."""
    header = read_header(data)

    # Walk the chunks the table lists; each must agree with its position.
    position = BLOCK_BYTES
    first4 = first2 = 0
    for number, blocks in enumerate(header.chunk_blocks):
        size = blocks * BLOCK_BYTES
        if not size or size > header.largest_chunk or position + size > len(data):
            raise ValueError("Invalid streamed preview chunk table")
        chunk = data[position : position + size]
        (
            chunk_sum,
            chunk_number,
            count,
            chunk_first4,
            new4,
            chunk_first2,
            new2,
            audio,
            video,
        ) = struct.unpack_from(">IHHHHHHII", chunk)

        first_frame = number * header.chunk_frames
        if header.audio_rate:
            # The samples for this chunk's frames, rounded as a running total.
            expected_audio = samples_for(first_frame + count) - samples_for(first_frame)
        else:
            expected_audio = 0
        # The frame table follows the header, the sound (padded to a word) and
        # the new dictionary entries.
        frame_table = CHUNK_HEADER + ((audio + 3) & ~3) + new4 * ENTRY4_BYTES + new2 * ENTRY2_BYTES
        if (
            word_sum(chunk[4:]) != chunk_sum
            or chunk_number != number
            or count != min(header.chunk_frames, header.frames - first_frame)
            or (chunk_first4, chunk_first2) != (first4, first2)
            or first4 + new4 > header.entries4
            or first2 + new2 > header.entries2
            or audio != expected_audio
            or frame_table + video > size
            or video < (count + 1) * 4
            or struct.unpack_from(">I", chunk, frame_table)[0] != (count + 1) * 4
            or struct.unpack_from(">I", chunk, frame_table + count * 4)[0] != video
        ):
            raise ValueError("Invalid streamed preview chunk " + str(number))
        first4 += new4
        first2 += new2
        position += size

    if position != len(data):
        raise ValueError("Streamed preview has trailing data")
    return header
