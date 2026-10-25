#!/usr/bin/env python3
"""The console's ARM60 preview decoder (src/preview_v5_arm.s) against the C
decoder (src/preview_v5.c) on random valid and damaged OM3DVID5 packets.

Run in the development container: armasm/armlink build the real object and
Unicorn runs it big-endian. Every frame must match the C decoder's pixels,
color cache and result; nothing may be written outside the frame, nor read
more than PREVIEW_V5_READ_AHEAD bytes past a packet."""
import ctypes
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_BIG_ENDIAN
from unicorn import arm_const

ROOT = Path(__file__).resolve().parent.parent
W, H = 160, 120
COLUMNS, BLOCKS = W // 4, (W // 4) * (H // 4)
FRAME = W * H * 2
READ_AHEAD = 1024
MOTION_RUN = 32  # longest MOTION run

CODE, STATE, PIXELS, STACK_TOP, STOP = 0x10000, 0x100000, 0x200000, 0x400000, 0x7FF000
GUARD = 0x1000  # mapped, checked bytes either side of the frame
PACKETS = 0x600000  # packet region: its last READ_AHEAD bytes end the mapping
PACKET_ROOM = 0x40000


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
            str(ROOT / "src/preview_v5_arm.s"),
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
    entry = int(
        re.search(r"preview_v5_decode\s+(?:0x)?([0-9A-Fa-f]+)", symbols.read_text()).group(1), 16
    )
    library = temp / "preview_v5.so"
    subprocess.run(
        [
            "cc",
            "-O1",
            "-shared",
            "-fPIC",
            "-I" + str(ROOT / "src"),
            str(ROOT / "src/preview_v5.c"),
            "-o",
            str(library),
        ],
        check=True,
    )
    return image.read_bytes(), entry, ctypes.CDLL(str(library))


class Arm:
    def __init__(self, image, entry):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_BIG_ENDIAN)
        self.entry = entry
        self.uc.mem_map(0, STACK_TOP + 0x1000)
        self.uc.mem_map(PACKETS, PACKET_ROOM)  # nothing is mapped after it
        self.uc.mem_map(STOP, 0x1000)
        self.uc.mem_write(CODE, image)
        self.reset()

    def reset(self):
        self.uc.mem_write(STATE, bytes(520))
        self.uc.mem_write(PIXELS - GUARD, b"\xa5" * GUARD + bytes(FRAME) + b"\xa5" * GUARD)

    def decode(self, packet, tail):
        address = PACKETS + PACKET_ROOM - READ_AHEAD - len(packet)
        self.uc.mem_write(address, packet + tail)
        regs = (
            arm_const.UC_ARM_REG_R0,
            arm_const.UC_ARM_REG_R1,
            arm_const.UC_ARM_REG_R2,
            arm_const.UC_ARM_REG_R3,
        )
        for reg, value in zip(regs, (STATE, address, len(packet), PIXELS)):
            self.uc.reg_write(reg, value)
        self.uc.reg_write(arm_const.UC_ARM_REG_SP, STACK_TOP)
        self.uc.reg_write(arm_const.UC_ARM_REG_R10, STACK_TOP - 0x10000)
        self.uc.reg_write(arm_const.UC_ARM_REG_LR, STOP)
        self.uc.emu_start(self.entry, STOP)
        result = self.uc.reg_read(arm_const.UC_ARM_REG_R0)
        assert (
            self.uc.reg_read(arm_const.UC_ARM_REG_R10) == STACK_TOP - 0x10000
        ), "r10 (stack limit) changed"
        guard = bytes(self.uc.mem_read(PIXELS - GUARD, GUARD)) + bytes(
            self.uc.mem_read(PIXELS + FRAME, GUARD)
        )
        assert guard == b"\xa5" * (2 * GUARD), "wrote outside the frame"
        return 0 if result == 0 else -1

    def pixels(self):
        return bytes(self.uc.mem_read(PIXELS, FRAME))

    def cache(self):
        return list(struct.unpack(">129I", self.uc.mem_read(STATE, 516)))


class C:
    class State(ctypes.Structure):
        _fields_ = [
            ("cache", ctypes.c_uint * 128),
            ("next", ctypes.c_uint),
            ("frames", ctypes.c_ulong),
        ]

    def __init__(self, library):
        self.frame = library.preview_v5_frame
        self.frame.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_ulong, ctypes.c_void_p)
        self.reset()

    def reset(self):
        self.state = C.State()
        self.buffer = ctypes.create_string_buffer(FRAME)

    def decode(self, packet, tail):
        data = ctypes.create_string_buffer(packet + tail, len(packet) + len(tail))
        return self.frame(ctypes.byref(self.state), data, len(packet), self.buffer)

    def pixels(self):
        return self.buffer.raw

    def cache(self):
        return list(self.state.cache) + [self.state.next]


def color(rng):
    if rng.random() < 0.5:
        return bytes([0x80 | rng.randrange(128)])
    value = rng.randrange(0x8000)
    return bytes([value >> 8, value & 255])


def motion_run(rng, block, room):
    """A vector and the longest run (up to room) whose sources all lie in the frame."""
    for _ in range(20):
        dx, dy = rng.randrange(-8, 8), rng.choice((rng.randrange(-8, 8), 0, -1, 1))
        run = 0
        while run < room:
            b = block + run
            sx, sy = (b % COLUMNS) * 4 + dx, (b // COLUMNS) * 4 + dy
            if not (0 <= sx <= W - 4 and 0 <= sy <= H - 4):
                break
            run += 1
        if run:
            return ((dx + 8) << 4) | (dy + 8), rng.randint(1, run) if rng.random() < 0.5 else run
    return None, 0


def packet(rng):
    out, block = bytearray(), 0
    while block < BLOCKS:
        room = min(64, BLOCKS - block)
        mode = rng.choices((0, 1, 2, 3), (2, 4, 2, 3))[0]
        run = rng.randint(1, room) if rng.random() < 0.4 else rng.randint(1, min(room, 4))
        if mode == 1:
            vector, run = motion_run(rng, block, min(room, MOTION_RUN))
            if not run:
                mode, run = 0, 1
        out.append(mode << 6 | (run - 1))
        if mode == 1:
            out.append(vector)
        elif mode == 2:
            out += color(rng)
        elif mode == 3:
            for _ in range(run):
                out += color(rng) + color(rng) + bytes(rng.randrange(256) for _ in range(4))
        block += run
    return bytes(out)


def main():
    rng = random.Random(5)
    with tempfile.TemporaryDirectory() as temp:
        image, entry, library = build(Path(temp))
        arm, c = Arm(image, entry), C(library)
        frames = 0
        for clip in range(4):
            arm.reset()
            c.reset()
            for frame in range(10):
                data, tail = packet(rng), bytes(rng.randrange(256) for _ in range(READ_AHEAD))
                assert arm.decode(data, tail) == 0 and c.decode(data, tail) == 0, (clip, frame)
                assert arm.pixels() == c.pixels(), f"clip {clip} frame {frame}: pixels differ"
                assert arm.cache() == c.cache(), f"clip {clip} frame {frame}: color cache differs"
                frames += 1
        rejected = 0
        for trial in range(150):
            arm.reset()
            c.reset()
            data = bytearray(packet(rng))
            kind = trial % 3
            if kind == 0:
                for _ in range(rng.randint(1, 4)):
                    data[rng.randrange(len(data))] = rng.randrange(256)
            elif kind == 1:
                del data[rng.randrange(1, len(data)) :]
            else:
                data += bytes(rng.randrange(256) for _ in range(rng.randint(1, 6)))
            tail = bytes(rng.randrange(256) for _ in range(READ_AHEAD))
            try:
                got = arm.decode(bytes(data), tail)
            except UcError as error:
                raise AssertionError(f"damaged packet {trial}: the decoder faulted ({error})")
            want = c.decode(bytes(data), tail)
            assert got == want, f"damaged packet {trial}: assembly {got}, C {want}"
            if want == 0:
                assert arm.pixels() == c.pixels(), f"damaged packet {trial}: pixels differ"
            rejected += want != 0
        assert rejected > 100, rejected
        # Motion just past each edge, alone and at the end of a run: both reject.
        edges = 0
        for column, row, dx, dy in (
            (COLUMNS - 1, 10, 1, 0),
            (COLUMNS - 1, 10, 7, -3),
            (0, 10, -1, 0),
            (5, 0, 0, -1),
            (5, H // 4 - 1, 2, 1),
            (20, H // 4 - 1, 0, 7),
        ):
            for run in (1, 3):
                start = row * COLUMNS + column - (run - 1 if dx >= 0 else 0)
                ops = bytearray()
                skip = start
                while skip:
                    n = min(64, skip)
                    ops.append(n - 1)
                    skip -= n
                ops += bytes([0x40 | (run - 1), ((dx + 8) << 4) | (dy + 8)])
                rest = BLOCKS - start - run
                while rest:
                    n = min(64, rest)
                    ops.append(n - 1)
                    rest -= n
                arm.reset()
                c.reset()
                tail = bytes(READ_AHEAD)
                assert arm.decode(bytes(ops), tail) == -1 and c.decode(bytes(ops), tail) == -1, (
                    column,
                    row,
                    dx,
                    dy,
                    run,
                )
                edges += 1
        # A MOTION run longer than 32 blocks is rejected even when its vector is valid.
        ops = bytearray([0x40 | 32, 0x88])
        rest = BLOCKS - 33
        while rest:
            n = min(64, rest)
            ops.append(n - 1)
            rest -= n
        arm.reset()
        c.reset()
        assert (
            arm.decode(bytes(ops), bytes(READ_AHEAD)) == -1
            and c.decode(bytes(ops), bytes(READ_AHEAD)) == -1
        )
    print(
        f"ARM60 preview decoder: {frames} random frames match the C decoder; "
        f"{rejected} of 150 damaged packets and {edges} out-of-frame vectors rejected by both"
    )


if __name__ == "__main__":
    main()
