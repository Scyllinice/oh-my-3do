#!/usr/bin/env python3
"""The console's OM3DVID7 decoder (om3dvid/src/om3dvid7_decoder_arm.s) against the C
decoder (om3dvid/src/om3dvid7_decoder.c), and its ARM60 clock counts.

    test_decoder_arm.py [CLIP.3dv ...] [--clocks]

Run in the development container: armasm/armlink build the real object and
Unicorn runs it big-endian. Random packets (all valid: the assembly trusts
packets om3dvid7_chunk accepted) and every frame of the given clips, drawn
into the screen bitmap or the TV picture, must leave the same pixels and map
as the C decoder, and so must full redraws from the map. (Full screen draws
each frame into the back buffer after a hardware copy of the front one, so
one bitmap stands for both.) Nothing may be written outside the buffers or
the stack. --clocks prints modelled ARM60 clocks per frame (see
tools/arm60-bench)."""
import ctypes
import importlib.util
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn import arm_const as A

# The library (om3dvid/) and the repository around it, which has the ARM60
# clock model (tools/arm60-bench).
LIBRARY = Path(__file__).resolve().parents[1]
ROOT = LIBRARY.parent
spec = importlib.util.spec_from_file_location("arm60", ROOT / "tools/arm60-bench/arm60.py")
arm60 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(arm60)

BLOCKS, FRAME, THUMB = 4800, 320 * 240 * 2, 160 * 120 * 2
CODE, STATE, MAP, SCREEN, THUMBBUF = 0x10000, 0x100000, 0x110000, 0x200000, 0x260000
DICT4, THUMB4, DICT2, THUMB2, PACKETS = 0x300000, 0x360000, 0x380000, 0x388000, 0x400000
STACK_TOP, STOP = 0x7F0000, 0x7FF000
GUARD = 0x1000
BLACK = 0x7FFFFFFF


def build(temp):
    obj, image, symbols = temp / "decoder.o", temp / "decoder.bin", temp / "decoder.sym"
    subprocess.run(
        [
            "armasm",
            "-bigend",
            "-fpu",
            "none",
            "-arch",
            "3",
            "-apcs",
            "3/32/nofp/swst",
            str(LIBRARY / "src/om3dvid7_decoder_arm.s"),
            "-o",
            str(obj),
        ],
        check=True,
    )
    subprocess.run(
        [
            "armlink",
            "-bin",
            "-ro-base",
            hex(CODE),
            "-nodebug",
            "-symbols",
            str(symbols),
            "-o",
            str(image),
            str(obj),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    text = symbols.read_text()
    entry = {
        name: int(re.search(name + r"\s+(?:0x)?([0-9A-Fa-f]+)", text).group(1), 16)
        for name in ("om3dvid7_decode_arm", "om3dvid7_render_arm", "om3dvid7_sum_arm")
    }
    library = temp / "om3dvid7_decoder.so"
    # The decoder's static packet loop, exposed for the damaged-packet test.
    wrapper = temp / "wrapper.c"
    wrapper.write_text(
        '#include "om3dvid7_decoder.c"\nint om3dvid7_decode_for_test(Om3dvid7Decoder *s,const unsigned char *d,'
        "unsigned long n,int p){return om3dvid7_decode_arm(s,d,n,p);}\n"
    )
    subprocess.run(
        [
            "cc",
            "-O1",
            "-shared",
            "-fPIC",
            "-I" + str(LIBRARY / "src"),
            "-I" + str(LIBRARY / "include"),
            str(wrapper),
            "-o",
            str(library),
        ],
        check=True,
    )
    return image.read_bytes(), entry, ctypes.CDLL(str(library))


class Header(ctypes.Structure):
    _fields_ = (
        [
            (n, ctypes.c_uint)
            for n in ("chunk_frames", "chunks", "preroll", "entries4", "entries2", "slots")
        ]
        + [(n, ctypes.c_ulong) for n in ("frames", "audio_rate", "max_chunk", "rate")]
        + [("table", ctypes.c_void_p)]
    )


class Chunk(ctypes.Structure):
    _fields_ = (
        [(n, ctypes.c_uint) for n in ("number", "frames", "first4", "count4", "first2", "count2")]
        + [(n, ctypes.c_ulong) for n in ("audio_samples", "video_bytes")]
        + [(n, ctypes.c_void_p) for n in ("audio", "entries4", "entries2", "offsets")]
    )


class State(ctypes.Structure):
    _fields_ = [
        ("screen", ctypes.c_void_p),
        ("thumb", ctypes.c_void_p),
        ("map", ctypes.c_void_p),
        ("dict4", ctypes.c_void_p),
        ("thumb4", ctypes.c_void_p),
        ("dict2", ctypes.c_void_p),
        ("thumb2", ctypes.c_void_p),
        ("entries4", ctypes.c_uint),
        ("entries2", ctypes.c_uint),
        ("limit4", ctypes.c_uint),
        ("limit2", ctypes.c_uint),
    ]


class Host:
    """The C decoder with its own memory."""

    def __init__(self, lib):
        self.lib = lib
        self.screen = ctypes.create_string_buffer(FRAME)
        self.thumb = ctypes.create_string_buffer(THUMB)
        self.map = (ctypes.c_ulong * (2 * BLOCKS))()
        self.dict4 = ctypes.create_string_buffer(8192 * 32)
        self.thumb4 = ctypes.create_string_buffer(8192 * 8)
        self.dict2 = ctypes.create_string_buffer(4096 * 8)
        self.thumb2 = (ctypes.c_ulong * 4096)()
        self.s = State(
            ctypes.addressof(self.screen),
            ctypes.addressof(self.thumb),
            ctypes.addressof(self.map),
            ctypes.addressof(self.dict4),
            ctypes.addressof(self.thumb4),
            ctypes.addressof(self.dict2),
            ctypes.addressof(self.thumb2),
            8192,
            4096,
            0,
            0,
        )
        lib.om3dvid7_reset(ctypes.byref(self.s))


class Arm:
    def __init__(self, image, entry):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN)
        self.entry = entry
        self.uc.mem_map(0, 0x800000)
        self.uc.mem_write(CODE, image)
        self.clocks = [0, 0, 0]
        self.counting = False
        self.uc.hook_add(UC_HOOK_CODE, self.hook)
        self.uc.mem_write(
            STATE, struct.pack(">7I", SCREEN, THUMBBUF, MAP, DICT4, THUMB4, DICT2, THUMB2)
        )
        for start, size in self.buffers():
            self.uc.mem_write(start - GUARD, b"\xa5" * GUARD + bytes(size) + b"\xa5" * GUARD)
        self.uc.mem_write(MAP, struct.pack(">%dI" % (2 * BLOCKS), *([BLACK, 0] * BLOCKS)))

    @staticmethod
    def buffers():
        return ((SCREEN, FRAME), (THUMBBUF, THUMB), (MAP, 8 * BLOCKS))

    def hook(self, uc, address, size, _):
        if self.counting:
            s, n, i = arm60.cost(uc, struct.unpack(">I", uc.mem_read(address, 4))[0])
            self.clocks[0] += s
            self.clocks[1] += n
            self.clocks[2] += i

    def define(self, host):
        """Entries are stored by the C code (om3dvid7_define); copy them."""
        self.uc.mem_write(DICT4, host.dict4.raw)
        self.uc.mem_write(THUMB4, host.thumb4.raw)
        self.uc.mem_write(DICT2, host.dict2.raw)
        self.uc.mem_write(THUMB2, struct.pack(">4096I", *host.thumb2))

    def call(self, name, *args):
        for reg, value in zip(
            (A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3), args
        ):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(A.UC_ARM_REG_SP, STACK_TOP)
        self.uc.reg_write(A.UC_ARM_REG_R10, STACK_TOP - 0x10000)
        self.uc.reg_write(A.UC_ARM_REG_LR, STOP)
        before = list(self.clocks)
        self.counting = True
        try:
            self.uc.emu_start(self.entry[name], STOP)
        except Exception:
            regs = [self.uc.reg_read(getattr(A, f"UC_ARM_REG_R{i}")) for i in range(13)]
            raise SystemExit(
                f"{name}: ARM fault at pc {self.uc.reg_read(A.UC_ARM_REG_PC):#x} "
                f"regs {[hex(r) for r in regs]}"
            )
        self.counting = False
        assert (
            self.uc.reg_read(A.UC_ARM_REG_R10) == STACK_TOP - 0x10000
        ), "r10 (stack limit) changed"
        assert self.uc.reg_read(A.UC_ARM_REG_SP) == STACK_TOP, "stack not balanced"
        for start, size in self.buffers():
            guard = bytes(self.uc.mem_read(start - GUARD, GUARD)) + bytes(
                self.uc.mem_read(start + size, GUARD)
            )
            assert guard == b"\xa5" * (
                2 * GUARD
            ), f"{name} wrote outside the buffer at {start:#x} (args {[hex(a) for a in args]})"
        return sum((self.clocks[k] - before[k]) * (1, 2, 1)[k] for k in range(3))

    def decode(self, address, size, picture):
        return self.call("om3dvid7_decode_arm", STATE, address, size, picture)

    def render(self, picture):
        return self.call("om3dvid7_render_arm", STATE, picture)

    def same(self, host, picture):
        arm_map = struct.unpack(">%dI" % (2 * BLOCKS), self.uc.mem_read(MAP, 8 * BLOCKS))
        for b in range(BLOCKS):
            w0 = host.map[2 * b]
            assert arm_map[2 * b] == w0, f"map word 0 of block {b} differs"
            if w0 & 0x80000000:
                assert arm_map[2 * b + 1] == host.map[2 * b + 1], f"map word 1 of block {b} differs"
        if picture:
            assert bytes(self.uc.mem_read(THUMBBUF, THUMB)) == host.thumb.raw, "TV picture differs"
        else:
            assert bytes(self.uc.mem_read(SCREEN, FRAME)) == host.screen.raw, "screen differs"


def run_clip(lib, arm, path, tv, clocks):
    data = Path(path).read_bytes()
    header = Header()
    block = ctypes.create_string_buffer(data[:2048], 2048)
    assert lib.om3dvid7_header(block, ctypes.byref(header)) == 0, "bad header"
    host = Host(lib)
    at, counts = 2048, []
    for c in range(header.chunks):
        size = struct.unpack_from(">H", data, 64 + 2 * c)[0] * 2048
        raw = ctypes.create_string_buffer(data[at : at + size] + bytes(4), size + 4)
        info = Chunk()
        assert (
            lib.om3dvid7_chunk(ctypes.byref(header), c, raw, size, ctypes.byref(info)) == 0
        ), f"chunk {c} rejected"
        lib.om3dvid7_define(ctypes.byref(host.s), ctypes.byref(info))
        arm.define(host)
        arm.uc.mem_write(PACKETS, data[at : at + size] + bytes(4))
        table = info.offsets - ctypes.addressof(raw)
        for k in range(info.frames):
            first, last = struct.unpack_from(">II", data, at + table + 4 * k)
            assert lib.om3dvid7_frame(ctypes.byref(host.s), ctypes.byref(info), k, tv) == 0
            counts.append(arm.decode(PACKETS + table + first, last - first, tv))
            arm.same(host, tv)
        at += size
    if clocks:
        Path(f'{path}.{"tv" if tv else "full"}.clocks').write_text(
            "".join(f"{n}\n" for n in counts)
        )
        mean = sum(counts) / len(counts)
        print(
            f'  {"TV" if tv else "full"}: mean {mean/1000:.1f}k clocks, worst {max(counts)/1000:.1f}k '
            f"(console about {arm60.CONSOLE_FACTOR*max(counts)/arm60.CLOCK*1000:.1f} ms)"
        )
    return counts


def random_packet(rng, limit4, limit2):
    words, block = [], 0
    while block < BLOCKS:
        n = min(BLOCKS - block, rng.choice([1, 1, 2, 3, 5, 17, 80, 81, 200, 1000]))
        kind = rng.randrange(4)
        if kind == 0 or (n == 1 and kind == 2):
            words.append(rng.randrange(limit4))
            n = 1
        elif kind == 1:
            words.append(0x2000 | (n - 1))
        elif kind == 2:
            words.append(0x4000 | (n - 1))
            words += [rng.randrange(limit4) for _ in range(n)]
        else:
            words.append(0x6000 | (n - 1))
            for _ in range(n):
                q = [rng.randrange(limit2) for _ in range(4)]
                hi, lo = q[0] << 12 | q[1], q[2] << 12 | q[3]
                words += [hi >> 8, (hi & 0xFF) << 8 | lo >> 16, lo & 0xFFFF]
        block += n
    return struct.pack(">%dH" % len(words), *words)


def random_chunk(rng, number, first4, first2):
    """A chunk with random entries and packets, checked by the C code."""
    frames, count4, count2 = rng.randrange(1, 5), rng.randrange(1, 300), rng.randrange(1, 300)
    packets = [random_packet(rng, first4 + count4, first2 + count2) for _ in range(frames)]
    offsets, at = [], 4 * (frames + 1)
    for p in packets:
        offsets.append(at)
        at += len(p)
    offsets.append(at)
    video = struct.pack(">%dI" % (frames + 1), *offsets) + b"".join(packets)
    body = struct.pack(">HHHHHHII", number, frames, first4, count4, first2, count2, 0, len(video))
    body += rng.randbytes(40 * count4) + rng.randbytes(12 * count2) + video
    body += bytes(-(len(body) + 4) % 2048)
    return frames, count4, count2, body


def random_test(lib, image, entry, rng, rounds):
    for _ in range(rounds):
        host = Host(lib)
        arm = Arm(image, entry)
        first4 = first2 = 0
        for number in range(3):
            frames, count4, count2, body = random_chunk(rng, number, first4, first2)
            data = struct.pack(">I", 0) + body
            total = len(data) // 2048
            header = Header(
                chunk_frames=frames,
                chunks=3,
                preroll=1,
                entries4=8192,
                entries2=4096,
                slots=1,
                frames=3 * frames,
                audio_rate=0,
                max_chunk=total * 2048,
                rate=1,
            )
            table = (ctypes.c_ubyte * (6))(*b"".join(struct.pack(">H", total) for _ in range(3)))
            header.table = ctypes.addressof(table)
            raw = ctypes.create_string_buffer(data, len(data) + 4)
            check = lib.om3dvid7_sum(ctypes.byref(raw, 4), len(data) - 4) & 0xFFFFFFFF
            ctypes.memmove(raw, struct.pack(">I", check), 4)
            data = raw.raw[: len(data)]
            info = Chunk()
            assert (
                lib.om3dvid7_chunk(ctypes.byref(header), number, raw, len(data), ctypes.byref(info))
                == 0
            ), "random chunk rejected"
            lib.om3dvid7_define(ctypes.byref(host.s), ctypes.byref(info))
            arm.define(host)
            # Odd placement: packets may start on either half of a word.
            base = PACKETS + rng.choice([0, 4, 8])
            arm.uc.mem_write(base, data + bytes(4))
            tableat = info.offsets - ctypes.addressof(raw)
            for k in range(frames):
                first, last = struct.unpack_from(">II", data, tableat + 4 * k)
                picture = rng.randrange(2)
                assert lib.om3dvid7_frame(ctypes.byref(host.s), ctypes.byref(info), k, picture) == 0
                arm.decode(base + tableat + first, last - first, picture)
                arm.same(host, picture)
                if rng.randrange(3) == 0:
                    picture ^= 1
                    lib.om3dvid7_render(ctypes.byref(host.s), picture)
                    arm.render(picture)
                    arm.same(host, picture)
            first4 += count4
            first2 += count2


def damaged_test(lib, image, entry, rng, rounds):
    """Damaged packets: the assembly must never fault or write outside its
    buffers, and must reject what breaks the structure, as the C decoder
    does (which also rejects undefined entry numbers)."""
    arm = Arm(image, entry)
    host = Host(lib)
    rejected = 0
    for _ in range(rounds):
        packet = bytearray(random_packet(rng, 8192, 4096))
        kind = rng.randrange(3)
        if kind == 0:
            packet = packet[: rng.randrange(0, len(packet) // 2 + 1) * 2]
        elif kind == 1:
            at = rng.randrange(len(packet) // 2) * 2
            packet[at : at + 2] = struct.pack(">H", rng.randrange(0x10000))
        else:
            packet += struct.pack(">H", rng.randrange(0x10000))
        picture = rng.randrange(2)
        arm.uc.mem_write(PACKETS, bytes(packet) + bytes(4))
        result = arm.call("om3dvid7_decode_arm", STATE, PACKETS, len(packet), picture)
        result = arm.uc.reg_read(A.UC_ARM_REG_R0)
        host.s.limit4, host.s.limit2 = 8192, 4096
        c = lib.om3dvid7_decode_for_test(ctypes.byref(host.s), bytes(packet), len(packet), picture)
        if c == 0:
            assert result == 0, "the assembly rejected a packet the C decoder accepted"
        elif result != 0:
            rejected += 1
    return rejected


def sum_test(image, entry, rng):
    """The assembly's word sum, in pieces, against the sum's definition."""
    arm = Arm(image, entry)
    for _ in range(20):
        words = rng.randrange(0, 900)
        data = rng.randbytes(4 * words)
        a = b = 0
        for (w,) in struct.iter_unpack(">I", data):
            a = (a + w) & 0xFFFFFFFF
            b = (b + a) & 0xFFFFFFFF
        arm.uc.mem_write(STATE, bytes(8))
        cut = 4 * rng.randrange(0, words + 1)
        arm.uc.mem_write(PACKETS, data)
        arm.call("om3dvid7_sum_arm", STATE, PACKETS, cut)
        arm.call("om3dvid7_sum_arm", STATE, PACKETS + cut, len(data) - cut)
        assert struct.unpack(">II", arm.uc.mem_read(STATE, 8)) == (a, b), "word sum differs"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    clocks = "--clocks" in sys.argv
    with tempfile.TemporaryDirectory() as temp:
        image, entry, lib = build(Path(temp))
        lib.om3dvid7_sum.restype = ctypes.c_ulong
        lib.om3dvid7_sum.argtypes = (ctypes.c_void_p, ctypes.c_ulong)
        random_test(lib, image, entry, random.Random(7), 6)
        print("random packets: ARM matches C")
        sum_test(image, entry, random.Random(11))
        print("word sum: ARM matches the definition")
        print(
            f"damaged packets: no faults, {damaged_test(lib, image, entry, random.Random(9), 300)} of 300 rejected"
        )
        for clip in args:
            for tv in (0, 1):
                run_clip(lib, Arm(image, entry), clip, tv, clocks)
            print(f"{clip}: ARM matches C in both pictures")


if __name__ == "__main__":
    main()
