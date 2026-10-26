#!/usr/bin/env python3
"""The Python module (om3dvid/python/om3dvid7.py): encoding through the
native library, with and without sound, checking files, and decoding them.

Usage: test_python.py [LIBRARY] [CLIP.3dv]
LIBRARY is the native library or its folder (default om3dvid/build, where
the library's own Makefile puts it); CLIP is a clip the C encoder test made,
which must check out too.
"""

import sys
import unittest
from pathlib import Path

# The library's folder (om3dvid/), wherever it is.
LIBRARY_FOLDER = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LIBRARY_FOLDER / "python"))
import om3dvid7

LIBRARY = LIBRARY_FOLDER / "build"
CLIP = None

FRAMES = 12


def test_frames(count=FRAMES):
    """Bands of a sliding gradient with a moving bright bar: enough change
    from frame to frame to fill several chunks."""
    width, height = om3dvid7.WIDTH, om3dvid7.HEIGHT
    frames = []
    for n in range(count):
        bar = (n * 7) % (width - 16)
        rows = []
        for band in range(height // 8):
            row = bytearray()
            for x in range(width):
                if bar <= x < bar + 16:
                    row += b"\xff\xff\xff"
                else:
                    row += bytes(((x + 3 * n) & 255, (band * 8 + n) & 255, (x ^ band) & 255))
            rows.append(bytes(row) * 8)
        frames.append(b"".join(rows))
    return frames


def test_sound(count=FRAMES):
    """A quiet square wave, signed 8-bit."""
    return bytes((20 if (i // 25) % 2 else 236) for i in range(om3dvid7.samples_for(count)))


class Om3dvid7Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = om3dvid7.load_library(LIBRARY)
        cls.frames = test_frames()
        cls.sound = test_sound()
        cls.clip = om3dvid7.encode(cls.frames, cls.sound, library=cls.library)
        cls.silent = om3dvid7.encode(cls.frames, None, library=cls.library)

    def test_word_sum(self):
        self.assertEqual(
            om3dvid7.word_sum(bytes([0, 0, 0, 1, 0, 0, 0, 2, 0x80, 0, 0, 0])), 0x80078003
        )
        self.assertIsNone(om3dvid7.word_sum(b"abc"))

    def test_default_settings(self):
        self.assertEqual(
            self.library.default_settings().as_dict(),
            {
                "rate": 200 * 1024,
                "read_rate": 275 * 1024,
                "slot_bytes": 640 * 1024,
                "max_chunk": 160 * 1024,
                "chunk_frames": 10,
                "entries4": 8192,
                "entries2": 4096,
                "cycle_cap": 230000,
            },
        )

    def test_clip_with_sound_checks_out(self):
        header = om3dvid7.check(self.clip.data)
        self.assertEqual(self.clip.data[:8], b"OM3DVID7")
        self.assertEqual((header.frames, self.clip.frames), (FRAMES, FRAMES))
        self.assertEqual(header.chunk_frames, 10)
        self.assertEqual(header.chunks, 2)
        self.assertTrue(header.has_sound)
        self.assertEqual(header.seconds, FRAMES / 30)
        self.assertEqual(len(header.chunk_blocks), header.chunks)
        self.assertEqual(self.clip.statistics["chunks"], header.chunks)
        self.assertEqual(self.clip.statistics["preroll"], header.preroll)
        self.assertEqual(set(self.clip.statistics), set(om3dvid7.STATISTICS))
        self.assertEqual(om3dvid7.read_header(self.clip.data), header)

    def test_sound_is_optional(self):
        header = om3dvid7.check(self.silent.data)
        self.assertFalse(header.has_sound)
        self.assertEqual(header.audio_rate, 0)

    def test_encoding_is_deterministic(self):
        encoder = om3dvid7.Encoder(FRAMES + 5, self.library)
        for frame in self.frames:
            encoder.add_frame(frame)
        self.assertEqual(encoder.finish(self.sound).data, self.clip.data)

    def test_bad_input(self):
        encoder = om3dvid7.Encoder(2, self.library)
        with self.assertRaises(ValueError):
            encoder.add_frame(b"\0" * 10)
        with self.assertRaises(ValueError):
            encoder.finish()  # no frames yet
        encoder.add_frame(self.frames[0])
        encoder.add_frame(self.frames[1])
        with self.assertRaises(ValueError):
            encoder.add_frame(self.frames[2])  # full
        with self.assertRaises(ValueError):
            encoder.finish(b"\0" * 5)  # sound for the wrong number of frames
        with self.assertRaises(ValueError):
            om3dvid7.Encoder(0, self.library)
        with self.assertRaises(ValueError):
            om3dvid7.Encoder(om3dvid7.MAX_FRAMES + 1, self.library)
        with self.assertRaises(FileNotFoundError):
            om3dvid7.load_library(LIBRARY_FOLDER / "no-such-folder")

    def test_damage_is_rejected(self):
        data = self.clip.data
        damaged = bytearray(data)
        damaged[om3dvid7.BLOCK_BYTES + 64] ^= 1  # inside the first chunk
        cases = {
            "chunk": bytes(damaged),
            "header": data[:20] + bytes([data[20] ^ 1]) + data[21:],
            "signature": b"OM3DVID6" + data[8:],
            "trailing data": data + bytes(om3dvid7.BLOCK_BYTES),
            "truncated": data[: -om3dvid7.BLOCK_BYTES],
            "header only": data[: om3dvid7.BLOCK_BYTES - 1],
        }
        for name, bad in cases.items():
            with self.subTest(name), self.assertRaises(ValueError):
                om3dvid7.check(bad)

    def test_decoding(self):
        reader = om3dvid7.Reader(self.clip.data, self.library)
        self.assertEqual((reader.frames, reader.audio_rate), (FRAMES, om3dvid7.AUDIO_RATE))
        # The sound is stored as it was given.
        self.assertEqual(reader.audio(), self.sound)
        decoded = list(reader)
        self.assertEqual(len(decoded), FRAMES)
        for original, frame in zip(self.frames, decoded):
            self.assertEqual(len(frame), om3dvid7.FRAME_BYTES)
            # Lossy (and this test picture is busier than real footage), but
            # each frame is far closer to its source than to a black frame.
            error = sum(abs(a - b) for a, b in zip(original[::97], frame[::97]))
            self.assertLess(error, sum(frame[::97]) / 2)
        # The same frames again through decode(), and the TV picture's size.
        self.assertEqual(list(om3dvid7.decode(self.clip.data, library=self.library)), decoded)
        number, small = reader.next(om3dvid7.TV)
        self.assertEqual((number, len(small)), (0, 160 * 120 * 3))  # looped back
        silent = om3dvid7.Reader(self.silent.data, self.library)
        self.assertFalse(silent.has_sound)
        self.assertEqual(silent.audio(), b"")

    def test_damaged_clip_fails_to_decode(self):
        damaged = bytearray(self.clip.data)
        damaged[-100] ^= 1  # in the last chunk
        reader = om3dvid7.Reader(bytes(damaged), self.library)
        with self.assertRaises(ValueError):
            for _ in range(FRAMES):
                reader.next()
        with self.assertRaises(ValueError):
            reader.audio()
        with self.assertRaises(ValueError):
            om3dvid7.Reader(self.clip.data[:-2048], self.library)

    def test_c_encoder_clip(self):
        if CLIP is None:
            self.skipTest("no clip from the C encoder test given")
        header = om3dvid7.check(Path(CLIP).read_bytes())
        self.assertGreater(header.frames, 0)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        LIBRARY = Path(sys.argv.pop(1))
    if len(sys.argv) > 1:
        CLIP = sys.argv.pop(1)
    unittest.main()
