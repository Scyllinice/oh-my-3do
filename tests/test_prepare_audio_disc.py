import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import wave

spec = importlib.util.spec_from_file_location(
    "audio_disc", Path(__file__).resolve().parents[1] / "tools/prepare-audio-disc.py"
)
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)


class AudioDiscTest(unittest.TestCase):
    def wav(self, folder, name, sectors, rate=44100):
        path = folder / name
        # Signed asymmetric samples detect endian or channel changes.
        pcm = struct.pack("<hh", -12345, 23456) * (588 * sectors)
        with wave.open(str(path), "wb") as out:
            out.setnchannels(2)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes(pcm)
        return pcm

    def test_pcm_and_recorded_gaps_survive_single_file_conversion(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = self.wav(root, "one.wav", 76)
            second = self.wav(root, "two.wav", 150)
            cue = root / "source.cue"
            cue.write_text(
                'FILE "one.wav" WAVE\n TRACK 01 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:00:37\nFILE "two.wav" WAVE\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:01:00\n'
            )
            before = cue.read_bytes()
            report = audio.convert(cue, root / "output")
            self.assertEqual((root / "output/output.bin").read_bytes(), first + second)
            self.assertEqual(
                (root / "output/output.cue").read_text(),
                'FILE "output.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:37\n  TRACK 02 AUDIO\n    INDEX 00 00:01:01\n    INDEX 01 00:02:01\n',
            )
            self.assertEqual(cue.read_bytes(), before)
            self.assertEqual(report["total_sectors"], 226)
            with self.assertRaises(ValueError):
                audio.convert(cue, root / "output")

    def test_refuses_resampling_and_unsupported_directives(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.wav(root, "one.wav", 75, 22050)
            cue = root / "source.cue"
            base = 'FILE "one.wav" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n'
            cue.write_text(base)
            with self.assertRaisesRegex(ValueError, "44.1"):
                audio.convert(cue, root / "output")
            self.assertFalse((root / "output").exists())
            self.wav(root, "one.wav", 75)
            cue.write_text(base + " PREGAP 00:02:00\n")
            with self.assertRaisesRegex(ValueError, "Unsupported"):
                audio.convert(cue, root / "output")
            cue.write_text(base.replace("one.wav", "../one.wav"))
            with self.assertRaisesRegex(ValueError, "beside"):
                audio.convert(cue, root / "output")

    def test_rejects_out_of_range_indexes_and_truncated_pcm(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.wav(root, "one.wav", 75)
            cue = root / "source.cue"
            cue.write_text('FILE "one.wav" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:01:00\n')
            with self.assertRaisesRegex(ValueError, "indexes"):
                audio.convert(cue, root / "output")
            cue.write_text(cue.read_text().replace("00:01:00", "00:00:00"))
            path = root / "one.wav"
            path.write_bytes(path.read_bytes()[:-4])
            with self.assertRaisesRegex(ValueError, "truncated"):
                audio.convert(cue, root / "output")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
