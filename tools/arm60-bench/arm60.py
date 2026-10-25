#!/usr/bin/env python3
"""Count ARM60 clocks per frame for the console's preview decoder.

    arm60.py BENCH.bin BENCH.sym PREVIEW_V5.so CLIP.3dv [FRAMES] [--profile]

Run by bench.sh, which builds BENCH.bin (bench.c + src/preview_v5_arm.s,
linked at 0x10000) and the host C decoder PREVIEW_V5.so.

Unicorn executes the real ARM code big-endian; every instruction is charged
by the ARM60 datasheet's S (sequential), N (non-sequential) and I (internal)
cycles, with N = 2 clocks (page-mode DRAM). It counts the CPU only: video DMA,
cel-engine drawing and other bus traffic are not modelled. On the console,
decoding measured about 1.28 times the counted time, which is the factor in
the "console" estimate. The decoded frames must match the C decoder exactly.
"""
import bisect
import collections
import ctypes
import re
import struct
import sys
import zlib

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn import arm_const as A

CLOCK = 12_500_000
CONSOLE_FACTOR = 1.28
BASE, VIDEO, STACK_TOP, STOP = 0x10000, 0x00800000, 0x01F00000, 0x01FFFF00
REGS = [getattr(A, f"UC_ARM_REG_R{i}") for i in range(13)]


def video_section(path):
    """Width, height, fps and the frame table plus packets of an OM3DVID5 clip.
    In the file the audio sits between the table and the packets."""
    data = open(path, "rb").read()
    if data[:8] != b"OM3DVID5":
        raise SystemExit(f"{path}: not an OM3DVID5 clip")
    width, height = struct.unpack_from(">HH", data, 8)
    frames, fps, video, audio = struct.unpack_from(">HHII", data, 20)
    payload = data[40:-8]
    table = (frames + 1) * 4
    return width, height, fps, frames, payload[:table] + payload[table + audio : audio + video]


def cost(uc, insn):
    """(S, N, I) cycles for one instruction, per the ARM60 datasheet."""
    cond = insn >> 28
    if cond != 0xE and not passes(uc, cond):
        return 1, 0, 0
    op = (insn >> 25) & 7
    if op in (0, 1):
        if op == 0 and (insn & 0x0FC000F0) == 0x00000090:  # MUL/MLA: early termination
            rs = (insn >> 8) & 15
            value = uc.reg_read(REGS[rs]) if rs < 13 else 0
            m = 1
            while m < 16 and (value >> (2 * m)) not in (0, 0xFFFFFFFF >> (2 * m)):
                m += 1
            return 1, 0, m
        s, n, i = 1, 0, 1 if op == 0 and insn & 0x10 else 0  # register-specified shift
        if (insn >> 12) & 15 == 15 and (insn >> 21) & 15 not in (8, 9, 10, 11):  # writes PC
            s, n = s + 1, n + 1
        return s, n, i
    if op in (2, 3):  # LDR/STR
        if insn & (1 << 20):
            return (2, 2, 1) if (insn >> 12) & 15 == 15 else (1, 1, 1)
        return 0, 2, 0
    if op == 4:  # LDM/STM
        k = bin(insn & 0xFFFF).count("1")
        if insn & (1 << 20):
            return (k + 1, 2, 1) if insn & (1 << 15) else (k, 1, 1)
        return k - 1, 2, 0
    return 2, 1, 0  # branches, SWI


def passes(uc, cond):
    f = uc.reg_read(A.UC_ARM_REG_CPSR)
    n, z, c, v = (f >> 31) & 1, (f >> 30) & 1, (f >> 29) & 1, (f >> 28) & 1
    return [
        z,
        not z,
        c,
        not c,
        n,
        not n,
        v,
        not v,
        c and not z,
        (not c) or z,
        n == v,
        n != v,
        (not z) and n == v,
        z or n != v,
        True,
        False,
    ][cond]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    profile = "--profile" in sys.argv
    image_path, symbols_path, decoder_path, clip = args[:4]
    width, height, fps, frames, video = video_section(clip)
    limit = min(frames, int(args[4])) if len(args) > 4 else frames
    symbols = {}
    for line in open(symbols_path):
        m = re.search(r"([A-Za-z_][\w$]*)\s+(?:0x)?([0-9A-Fa-f]{6,8})\b", line)
        if m:
            symbols.setdefault(m.group(1), int(m.group(2), 16))
    image = open(image_path, "rb").read()

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, 32 * 1024 * 1024)
    uc.mem_write(BASE, image)
    uc.mem_write(VIDEO, video + bytes(1024))  # the decoder's read-ahead
    uc.mem_write(symbols["bench_video"], struct.pack(">I", VIDEO))
    counts = [0, 0, 0]
    functions = sorted((a, n) for n, a in symbols.items() if BASE <= a < BASE + len(image))
    starts = [a for a, _ in functions]
    by_function = collections.Counter()

    def hook(uc, address, size, _):
        s, n, i = cost(uc, struct.unpack(">I", uc.mem_read(address, 4))[0])
        counts[0] += s
        counts[1] += n
        counts[2] += i
        if profile:
            by_function[functions[bisect.bisect_right(starts, address) - 1][1]] += s + 2 * n + i

    uc.hook_add(UC_HOOK_CODE, hook)

    def call(name, arg=0):
        uc.reg_write(A.UC_ARM_REG_R0, arg)
        uc.reg_write(A.UC_ARM_REG_SP, STACK_TOP)
        uc.reg_write(A.UC_ARM_REG_R10, STACK_TOP - 256 * 1024)  # APCS stack limit
        uc.reg_write(A.UC_ARM_REG_LR, STOP)
        uc.emu_start(symbols[name], STOP)
        r = uc.reg_read(A.UC_ARM_REG_R0)
        return r - (1 << 32) if r & 0x80000000 else r

    class State(ctypes.Structure):
        _fields_ = [
            ("cache", ctypes.c_uint * 128),
            ("next", ctypes.c_uint),
            ("frames", ctypes.c_ulong),
        ]

    host = ctypes.CDLL(decoder_path)
    host.preview_v5_frame.argtypes = (
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_ulong,
        ctypes.c_void_p,
    )
    state, pixels = State(), ctypes.create_string_buffer(width * height * 2)

    call("bench_reset")
    clocks, arm_crc, host_crc = [], 0, 0
    for f in range(limit):
        before = list(counts)
        if call("bench_frame", f) != 0:
            raise SystemExit(f"frame {f}: the ARM decoder rejected the packet")
        clocks.append(sum((counts[k] - before[k]) * (1, 2, 1)[k] for k in range(3)))
        arm_crc = zlib.crc32(
            bytes(uc.mem_read(symbols["bench_pixel_words"], width * height * 2)), arm_crc
        )
        first, last = struct.unpack_from(">II", video, f * 4)
        if host.preview_v5_frame(
            ctypes.byref(state), video[first:last] + bytes(1024), last - first, pixels
        ):
            raise SystemExit(f"frame {f}: the C decoder rejected the packet")
        host_crc = zlib.crc32(pixels.raw, host_crc)

    frame_clocks = CLOCK / fps
    mean = sum(clocks) / len(clocks)
    worst = max(clocks)
    print(f"{clip}: {len(clocks)} frames at {width}x{height}, {fps} fps")
    print(
        f"  mean {mean / 1000:.0f}k clocks ({100 * mean / frame_clocks:.1f}% of a frame), "
        f"worst {worst / 1000:.0f}k ({100 * worst / frame_clocks:.1f}%, frame {clocks.index(worst)})"
    )
    print(
        f"  console estimate: {CONSOLE_FACTOR * mean / CLOCK * 1000:.1f} ms mean, "
        f"{CONSOLE_FACTOR * worst / CLOCK * 1000:.1f} ms worst, of {1000 / fps:.1f} ms per frame"
    )
    print(
        f'  output {"matches" if arm_crc == host_crc else "DIFFERS FROM"} the C decoder '
        f"(crc {arm_crc:08x} / {host_crc:08x})"
    )
    if profile:
        total = sum(by_function.values())
        for name, n in by_function.most_common(10):
            print(f"  {name:28s} {100 * n / total:5.1f}%")
    if arm_crc != host_crc:
        sys.exit(1)


if __name__ == "__main__":
    main()
