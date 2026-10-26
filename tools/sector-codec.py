#!/usr/bin/env python3
"""Read-only, bounded-memory hashing of cooked ISOs as ECMA-130 Mode 1 tracks.

A "cooked" .iso holds only the 2048 data bytes of each CD sector. The disc
itself stores 2352 bytes per sector (Mode 1): a sync pattern, a header with
the sector's address, the data, a checksum (EDC) and error-correction parity
(ECC). Disc databases such as Redump list the SHA-1 of the full 2352-byte
track, so to match an .iso against them we rebuild each full sector and hash
the result, without ever writing it to disk.

The work is done by a native library (sector-codec.c, built by
build-sector-codec.py) when one is present, and by the slower pure-Python
encode_python otherwise. Both produce identical bytes.
"""
import ctypes
import hashlib
import os
from pathlib import Path
import struct
import sys
import time

COOKED = 2048
RAW = 2352
MAX_SECTORS = 449850  # Last generated address is 99:59:74.

# How many sectors are encoded per call (and the native buffer's size).
BATCH = 512

# Data tracks start two seconds (150 sectors) into the disc, so sector 0 of
# the .iso has address 00:02:00.
PREGAP_SECTORS = 150
SECTORS_PER_SECOND = 75
SECTORS_PER_MINUTE = 75 * 60

SYNC_PATTERN = b"\0" + b"\xff" * 10 + b"\0"
MODE_1 = 1

# Where each part sits inside a 2352-byte sector.
HEADER_OFFSET = 12  # 3 address bytes and the mode byte, after the sync
DATA_OFFSET = 16
EDC_OFFSET = 2064  # the 4-byte checksum, followed by 8 zero bytes
P_PARITY_OFFSET = 2076
Q_PARITY_OFFSET = 2248

# The EDC is a CRC-32 over the sync, header and data, using ECMA-130's
# polynomial (bit-reversed form), stored little-endian.
EDC_POLYNOMIAL = 0xD8018001

# The ECC field is a Reed-Solomon product code in GF(2^8), with
# x^8 + x^4 + x^3 + x^2 + 1 (0x11D) as the field's polynomial.
GALOIS_POLYNOMIAL = 0x11D

# The two parity passes. Each reads bytes starting at the header as a grid
# of 16-bit words and adds two parity bytes per column. Each entry:
#   (columns, rows, column step, step to the next row, parity offset)
# P parity: 86 columns of 24 bytes, read straight down; covers the 2064
#   bytes from the header through the zero bytes after the EDC.
# Q parity: 52 diagonals of 43 bytes, each wrapping around the grid;
#   covers those bytes plus the P parity (2236 bytes).
PARITY_PASSES = (
    (86, 24, 2, 86, P_PARITY_OFFSET),
    (52, 43, 86, 88, Q_PARITY_OFFSET),
)


def build_tables():
    """Precompute the lookup tables the encoder needs.

    Returns (crc_table, double, divide_by_three):
      crc_table[b]       the EDC CRC of a single byte b
      double[b]          b multiplied by 2 in the Galois field
      divide_by_three[b] b divided by 3 in the Galois field (3 = 2 + 1,
                         so the value v with double[v] ^ v == b)
    """
    crc_table = []
    double = []
    divide_by_three = [0] * 256

    for value in range(256):
        crc = value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ EDC_POLYNOMIAL
            else:
                crc = crc >> 1
        crc_table.append(crc)

        product = value << 1
        if value & 128:
            product ^= GALOIS_POLYNOMIAL
        product &= 255
        double.append(product)
        divide_by_three[product ^ value] = value

    return crc_table, double, divide_by_three


CRC, DOUBLE, DIVIDE_THREE = build_tables()


def check_range(data, first):
    """Return how many sectors `data` holds, or raise ValueError when it isn't
    a whole number of cooked sectors that fits in one batch and on a disc."""
    count, remainder = divmod(len(data), COOKED)
    if remainder or first < 0 or count > BATCH or first + count > MAX_SECTORS:
        raise ValueError("Invalid cooked-sector range")
    return count


def bcd(value):
    """Binary-coded decimal: 42 becomes 0x42."""
    return (value // 10) * 16 + value % 10


def sector_header(sector):
    """The 16 bytes before a sector's data: the sync pattern, then its
    address as minutes, seconds and frames in BCD, then the mode."""
    address = sector + PREGAP_SECTORS
    minutes = address // SECTORS_PER_MINUTE
    seconds = address // SECTORS_PER_SECOND % 60
    frames = address % SECTORS_PER_SECOND
    return SYNC_PATTERN + bytes((bcd(minutes), bcd(seconds), bcd(frames), MODE_1))


def edc(sector_bytes):
    """The EDC checksum of a sector's sync, header and data."""
    crc = 0
    for value in sector_bytes:
        crc = (crc >> 8) ^ CRC[(crc ^ value) & 255]
    return crc


def add_parity(raw, base):
    """Fill in the P and then Q parity of the sector starting at raw[base].

    For each column, s0 is the XOR of its bytes and s1 their weighted sum
    in the Galois field (each byte multiplied by a power of two that
    depends on its row). The two parity bytes
    p0 and p1 are chosen so the column checks out with them appended.
    """
    for columns, rows, column_step, row_step, parity_offset in PARITY_PASSES:
        grid_size = columns * rows
        for column in range(columns):
            # Even and odd columns are the high and low bytes of one 16-bit word.
            position = (column // 2) * column_step + column % 2
            s0 = 0
            s1 = 0
            for _ in range(rows):
                value = raw[base + HEADER_OFFSET + position]
                s0 ^= value
                s1 = DOUBLE[s1 ^ value]
                position = (position + row_step) % grid_size

            p0 = DIVIDE_THREE[DOUBLE[s1] ^ s0]
            p1 = p0 ^ s0
            raw[base + parity_offset + column] = p0
            raw[base + parity_offset + columns + column] = p1


def encode_python(data, first=0):
    """Portable fallback. Standard headers only; no guessing gaps or offsets.

    `data` is a whole number of 2048-byte sectors (at most BATCH) and `first`
    is the first one's sector number in the image. Returns the full 2352-byte
    sectors as a bytearray.
    """
    count = check_range(data, first)
    raw = bytearray(count * RAW)

    for n in range(count):
        base = n * RAW
        raw[base : base + DATA_OFFSET] = sector_header(first + n)
        raw[base + DATA_OFFSET : base + EDC_OFFSET] = data[n * COOKED : (n + 1) * COOKED]

        checksum = edc(memoryview(raw)[base : base + EDC_OFFSET])
        struct.pack_into("<I", raw, base + EDC_OFFSET, checksum)

        # The 8 bytes after the EDC stay zero.
        add_parity(raw, base)

    return raw


def library_file_name():
    """The native library's file name on this platform."""
    if os.name == "nt":
        return "sector-codec-win64.dll"
    if sys.platform == "darwin":
        return "sector-codec.dylib"
    return "sector-codec.so"


def native_encoder(directory=None):
    """Load only the named accelerator beside this tool (or explicit test dir).

    Without `directory`, looks beside this script (a media-tools package) and
    then in build/media-native/ (a checkout). Returns a function with the
    same arguments as encode_python, or None when no usable library is found.
    The function's result is a view of a shared buffer that the next call
    overwrites, so use it before encoding again.
    """
    name = library_file_name()
    folder = Path(__file__).resolve().parent
    if directory:
        candidates = [Path(directory) / name]
    else:
        candidates = [folder / name, folder.parent / "build/media-native" / name]

    path = next((p for p in candidates if p.is_file()), None)
    if path is None:
        return None

    try:
        library = ctypes.CDLL(str(path.resolve()))
        library.oh3do_sector_abi.restype = ctypes.c_uint
        if library.oh3do_sector_abi() != 1:
            raise ValueError("Unsupported sector accelerator ABI")
        encode = library.oh3do_mode1_encode
        encode.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)
        encode.restype = ctypes.c_int
    except (OSError, AttributeError) as error:
        print(f"Sector accelerator unavailable ({error}); using portable Python.", file=sys.stderr)
        return None

    buffer = ctypes.create_string_buffer(BATCH * RAW)

    def run(data, first=0):
        count = check_range(data, first)
        if encode(data, count, first, buffer):
            raise ValueError("Sector accelerator rejected input")
        return memoryview(buffer).cast("B")[: count * RAW]

    return run


def digests(path, encoder=None):
    """Compute source and reconstructed SHA-1 together from one file read.

    Returns the file's size, modification time and SHA-1, plus the size and
    SHA-1 of the Mode 1 track rebuilt from it. Raises ValueError if the file
    isn't a whole number of sectors or changes while it is being read.
    `encoder` overrides the automatic choice (for tests). Prints progress
    every five seconds, since large images take a while in pure Python.
    """
    before = path.stat()
    count, remainder = divmod(before.st_size, COOKED)
    if remainder or not 0 < count <= MAX_SECTORS:
        raise ValueError("Not a whole-sector cooked CD image")

    encode = encoder or native_encoder() or encode_python
    original = hashlib.sha1()
    reconstructed = hashlib.sha1()
    first = 0
    last_progress = time.monotonic()

    with path.open("rb") as stream:
        while first < count:
            wanted = min(BATCH, count - first) * COOKED
            data = stream.read(wanted)
            if len(data) != wanted:
                raise ValueError(f"File changed while reading: {path}")

            original.update(data)
            reconstructed.update(encode(data, first))
            first += len(data) // COOKED

            now = time.monotonic()
            if now - last_progress >= 5:
                percent = 100 * first // count
                print(f"  Reconstructing Mode 1 checksum: {percent}% ({path.name})", flush=True)
                last_progress = now

        # Anything left means the file grew after we checked its size.
        if stream.read(1):
            raise ValueError(f"File changed while reading: {path}")

    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"File changed while reading: {path}")

    return dict(
        size=before.st_size,
        mtime_ns=before.st_mtime_ns,
        sha1=original.hexdigest(),
        mode1_raw=dict(
            size=count * RAW, sha1=reconstructed.hexdigest(), format="MODE1/2352", first_lba=0
        ),
    )
