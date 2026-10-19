#!/usr/bin/env python3
"""Read-only, bounded-memory hashing of cooked ISOs as ECMA-130 Mode 1 tracks."""
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
BATCH = 512


def tables():
    crc_table, double, divide_three = [], [], [0] * 256
    for value in range(256):
        crc = value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xd8018001 if crc & 1 else 0)
        crc_table.append(crc)
        product = ((value << 1) ^ (0x11d if value & 128 else 0)) & 255
        double.append(product)
        divide_three[product ^ value] = value
    return crc_table, double, divide_three


CRC, DOUBLE, DIVIDE_THREE = tables()


def encode_python(data, first=0):
    """Portable fallback. Standard headers only; no guessing gaps or offsets."""
    count, remainder = divmod(len(data), COOKED)
    if remainder or first < 0 or count > BATCH or first + count > MAX_SECTORS:
        raise ValueError('Invalid cooked-sector range')
    raw = bytearray(count * RAW)
    for n in range(count):
        base = n * RAW
        frame = first + n + 150
        bcd = lambda value: (value // 10) * 16 + value % 10
        raw[base:base+16] = b'\0' + b'\xff'*10 + b'\0' + bytes((
            bcd(frame // 4500), bcd(frame // 75 % 60), bcd(frame % 75), 1))
        raw[base+16:base+2064] = data[n*COOKED:(n+1)*COOKED]
        crc = 0
        for value in memoryview(raw)[base:base+2064]:
            crc = (crc >> 8) ^ CRC[(crc ^ value) & 255]
        struct.pack_into('<I', raw, base+2064, crc)
        for columns, rows, step, stride, output in ((86,24,2,86,2076),(52,43,86,88,2248)):
            for column in range(columns):
                position = (column // 2) * step + column % 2
                s0 = s1 = 0
                for _ in range(rows):
                    value = raw[base+12+position]
                    s0 ^= value
                    s1 = DOUBLE[s1 ^ value]
                    position = (position + stride) % (columns * rows)
                p0 = DIVIDE_THREE[DOUBLE[s1] ^ s0]
                raw[base+output+column] = p0
                raw[base+output+columns+column] = p0 ^ s0
    return raw


def native_encoder(directory=None):
    """Load only the named accelerator beside this tool (or explicit test dir)."""
    name = ('sector-codec-win64.dll' if os.name == 'nt' else
            'sector-codec.dylib' if sys.platform == 'darwin' else 'sector-codec.so')
    folder = Path(__file__).resolve().parent
    candidates = [Path(directory)/name] if directory else [folder/name, folder.parent/'build/media-native'/name]
    path = next((p for p in candidates if p.is_file()), candidates[0])
    if not path.is_file():
        return None
    try:
        library = ctypes.CDLL(str(path.resolve()))
        library.oh3do_sector_abi.restype = ctypes.c_uint
        if library.oh3do_sector_abi() != 1:
            raise ValueError('Unsupported sector accelerator ABI')
        encode = library.oh3do_mode1_encode
        encode.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)
        encode.restype = ctypes.c_int
    except (OSError, AttributeError) as error:
        print(f'Sector accelerator unavailable ({error}); using portable Python.', file=sys.stderr)
        return None
    buffer = ctypes.create_string_buffer(BATCH * RAW)
    def run(data, first=0):
        count, remainder = divmod(len(data), COOKED)
        if remainder or first < 0 or count > BATCH or first + count > MAX_SECTORS:
            raise ValueError('Invalid cooked-sector range')
        if encode(data, count, first, buffer):
            raise ValueError('Sector accelerator rejected input')
        return memoryview(buffer).cast('B')[:count*RAW]
    return run


def digests(path, encoder=None):
    """Compute source and reconstructed SHA-1 together from one file read."""
    before = path.stat()
    count, remainder = divmod(before.st_size, COOKED)
    if remainder or not 0 < count <= MAX_SECTORS:
        raise ValueError('Not a whole-sector cooked CD image')
    encode = encoder or native_encoder() or encode_python
    original, reconstructed = hashlib.sha1(), hashlib.sha1()
    first, last_progress = 0, time.monotonic()
    with path.open('rb') as stream:
        while first < count:
            wanted = min(BATCH, count - first) * COOKED
            data = stream.read(wanted)
            if len(data) != wanted:
                raise ValueError(f'File changed while reading: {path}')
            original.update(data)
            reconstructed.update(encode(data, first))
            first += len(data) // COOKED
            now = time.monotonic()
            if now - last_progress >= 5:
                print(f'  Reconstructing Mode 1 checksum: {100*first//count}% ({path.name})', flush=True)
                last_progress = now
        if stream.read(1):
            raise ValueError(f'File changed while reading: {path}')
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f'File changed while reading: {path}')
    return dict(size=before.st_size, mtime_ns=before.st_mtime_ns, sha1=original.hexdigest(),
                mode1_raw=dict(size=count*RAW, sha1=reconstructed.hexdigest(),
                               format='MODE1/2352', first_lba=0))
