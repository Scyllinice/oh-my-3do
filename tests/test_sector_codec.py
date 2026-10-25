#!/usr/bin/env python3
"""Sector vectors cross-checked with an external encoder, no game images."""
import hashlib
import importlib.util
from pathlib import Path
import random
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("sector_codec", ROOT / "tools/sector-codec.py")
codec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codec)


class SectorCodecTest(unittest.TestCase):
    # User data is bytes(range(256))*8. These SHA-256 values were independently
    # checked with Neill Corlett's UNECM, source SHA-256
    # f3b90e3082502bb26c9e790222ab2d1c9b3d19860b0a3442f52e87498e920f77.
    # No reference implementation or commercial disc bytes are bundled.
    VECTORS = {
        0: "f6a94f686e87d4dd2816703689017aa16078564724f033e97eb0faa23cb1adb3",
        74: "c18494cc4271758ae87e799694b431d314abc0960171f560aabdf672caeeaf9c",
        75: "dfcaf6783efe58e936170961bdc4921a29bb2db891b9b476c7bb3e8b355a1d75",
        4349: "a791d57e7d4889172e07207f4caefbf68670b665160f16be9776ed9f32c687f9",
        4350: "38397ab18afe3faf6a9f95a34d0e6f47072034176fa3c5a20b21904d10e4c3cd",
        449849: "a2c37c79067dab0210b631451e353c809ef2ec709e8d0eec2a7ab8395f2d85d5",
    }

    def test_independent_vectors_and_portable_native_agreement(self):
        native = codec.native_encoder()
        self.assertIsNotNone(native, "Build the PC sector accelerator before running this test")
        data = bytes(range(256)) * 8
        for first, expected in self.VECTORS.items():
            with self.subTest(first=first):
                portable = codec.encode_python(data, first)
                accelerated = bytes(native(data, first))
                self.assertEqual(hashlib.sha256(portable).hexdigest(), expected)
                self.assertEqual(portable, accelerated)
                self.assertEqual(accelerated[16:2064], data)
                self.assertEqual(accelerated[2068:2076], b"\0" * 8)

    def test_nonrepeating_data_and_multiple_sectors_agree(self):
        data = random.Random(42).randbytes(2048 * 17)
        native = codec.native_encoder()
        raw = bytes(native(data, 73))
        self.assertEqual(raw, codec.encode_python(data, 73))
        self.assertEqual(
            hashlib.sha256(raw).hexdigest(),
            "3a8f5ef7c87662d57560116e01e42d7bb4b8585fac0cdc11d2b8f5ddab1b4693",
        )

    def test_bounded_ranges(self):
        for encode in (codec.encode_python, codec.native_encoder()):
            for data, first in (
                (b"x", 0),
                (b"\0" * 2048, -1),
                (b"\0" * 2048, 449850),
                (b"\0" * (2048 * 513), 0),
            ):
                with self.assertRaises(ValueError):
                    encode(data, first)
            self.assertEqual(bytes(encode(b"")), b"")

    def test_digest_spans_batch_boundary_without_modifying_source(self):
        data = random.Random(7).randbytes(2048 * 513)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "disc.iso"
            path.write_bytes(data)
            before = path.stat()
            result = codec.digests(path)
            expected = hashlib.sha1()
            expected.update(codec.encode_python(data[: 2048 * 512]))
            expected.update(codec.encode_python(data[2048 * 512 :], 512))
            self.assertEqual(result["sha1"], hashlib.sha1(data).hexdigest())
            self.assertEqual(result["mode1_raw"]["sha1"], expected.hexdigest())
            self.assertEqual(result["mode1_raw"]["size"], 513 * 2352)
            self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(path.read_bytes(), data)

    def test_bad_sizes_and_file_change_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "disc.iso"
            for data in (b"", b"not whole sectors"):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    codec.digests(path)
            path.write_bytes(b"\0" * 2048)

            def changed(data, first):
                path.write_bytes(b"\1" * 4096)
                return codec.encode_python(data, first)

            with self.assertRaisesRegex(ValueError, "File changed"):
                codec.digests(path, changed)

    def test_missing_accelerator_is_optional(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(codec.native_encoder(Path(d)))
            path = Path(d) / "disc.iso"
            path.write_bytes(b"\0" * 2048)
            self.assertEqual(
                codec.digests(path, codec.encode_python)["mode1_raw"],
                codec.digests(path, codec.native_encoder())["mode1_raw"],
            )


if __name__ == "__main__":
    unittest.main()
