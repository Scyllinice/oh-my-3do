#!/usr/bin/env python3
import hashlib, importlib.util, json, tempfile, unittest, zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "manifest", Path(__file__).resolve().parents[1] / "tools/write-update-manifest.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def checksum(self, p):
        p.with_name(p.name + ".sha256").write_text(
            hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n"
        )

    def test_menu_rebuild_replaces_manifest_and_stale_sidecars_fail(self):
        p = self.root / "boot.iso"
        p.write_bytes(b"first")
        self.checksum(p)
        target = module.write_manifest("menu", p, "1.0.2")
        first = json.loads(target.read_text())
        p.write_bytes(b"rebuilt")
        with self.assertRaises(ValueError):
            module.write_manifest("menu", p, "1.0.2")
        self.assertEqual(json.loads(target.read_text()), first)
        self.checksum(p)
        module.write_manifest("menu", p, "1.0.2")
        self.assertNotEqual(json.loads(target.read_text())["sha256"], first["sha256"])

    def test_desktop_checks_embedded_binary_and_platform(self):
        binary = b"actual executable"
        p = self.root / "OhMy3DO-Library-Linux-x64-v1.0.2.zip"
        with zipfile.ZipFile(p, "w") as z:
            z.writestr("OhMy3DO-Library/OhMy3DOLibrary", binary)
            z.writestr(
                "OhMy3DO-Library/BUILD.json",
                json.dumps(
                    dict(
                        app_version="1.0.2",
                        platform="Linux-x64",
                        binary_sha256=hashlib.sha256(binary).hexdigest(),
                    )
                ),
            )
        self.checksum(p)
        target = module.write_manifest("desktop", p, platform="Linux-x64")
        self.assertEqual(
            json.loads(target.read_text())["binary_sha256"], hashlib.sha256(binary).hexdigest()
        )
        with self.assertRaises(ValueError):
            module.write_manifest("desktop", p, platform="Windows-x64")
        with self.assertRaises(ValueError):
            module.write_manifest("desktop", p, "1.0.3", platform="Linux-x64")


if __name__ == "__main__":
    unittest.main()
