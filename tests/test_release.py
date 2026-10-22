#!/usr/bin/env python3
"""Release boundaries: independent versions, asset isolation and non-destructive staging."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location('release', Path(__file__).resolve().parents[1] / 'tools/release.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class Releases(unittest.TestCase):
    def fixture(self, root):
        files = {
            'src/main.c': '#define MENU_VERSION "1.2.3"\n',
            'tools/organize-library.py': "SCANNER_VERSION = '1.2.3'\n",
            'assets/media-tools-README.txt': 'Oh My 3DO media tools (menu v1.2.3)\n',
            'desktop/Cargo.toml': 'version = "3.4.5"\n',
            'desktop/shell/Cargo.toml': 'version = "3.4.5"\n',
            'desktop/Cargo.lock': 'name = "oh-my-3do-library"\nversion = "3.4.5"\n',
            'desktop/shell/Cargo.lock': 'name = "oh-my-3do-desktop"\nversion = "3.4.5"\nname = "oh-my-3do-library"\nversion = "3.4.5"\n',
            'desktop/shell/tauri.conf.json': '{"version":"3.4.5"}',
            'ohmy3do.ini': 'defaults', 'LICENSE': 'license', 'THIRD_PARTY.md': 'notices',
        }
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        iso = root / 'iso'
        iso.mkdir()
        for name in ('boot.iso', 'menu-demo.iso', 'media-tools.zip', 'ohmy3do.ini'):
            (iso / name).write_bytes(b'defaults' if name == 'ohmy3do.ini' else name.encode())
        with zipfile.ZipFile(iso / 'media-tools.zip', 'w') as archive:
            for name, source in [('organize-library.py', 'tools/organize-library.py'), ('README.txt', 'assets/media-tools-README.txt'), ('ohmy3do.ini', 'ohmy3do.ini')]:
                archive.writestr(name, (root / source).read_bytes())
        zipname = 'OhMy3DO-Library-Linux-x64-v3.4.5.zip'
        with zipfile.ZipFile(iso / zipname, 'w') as archive:
            archive.writestr('OhMy3DO-Library/BUILD.json', json.dumps({'app_version': '3.4.5', 'platform': 'Linux-x64'}))
        (iso / 'OhMy3DO-Library-Linux-x64-v3.4.5-Install.run').write_bytes(b'installer')
        for path in list(iso.iterdir()):
            if path.name != 'ohmy3do.ini':
                path.with_name(path.name + '.sha256').write_text(release.digest(path) + '  ' + path.name + '\n')
        for mode in ('menu', 'demo'):
            audit = root / f'build/console-{mode}/source.sha256'
            audit.parent.mkdir(parents=True)
            audit.write_text(release.digest(root / 'src/main.c') + '  /workspace/src/main.c\n')

    def test_versions_can_differ_but_inconsistent_component_settings_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            self.assertEqual(release.versions(root), {'menu': '1.2.3', 'desktop': '3.4.5'})
            (root / 'desktop/shell/tauri.conf.json').write_text('{"version":"1.2.3"}')
            with self.assertRaisesRegex(ValueError, 'Tauri'):
                release.versions(root)

    def test_staging_isolates_streams_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            with patch.object(release.subprocess, 'check_output', side_effect=lambda args, **kw: 'abc123\n' if args[1] == 'rev-parse' else ''):
                for component in ('menu', 'desktop'):
                    out = root / ('staged-' + component)
                    release.stage(component, ['Linux-x64'], out, root)
                    names = {p.name for p in out.iterdir()}
                    self.assertEqual('boot.iso' in names, component == 'menu')
                    self.assertEqual(any(name.startswith('OhMy3DO-Library') for name in names), component == 'desktop')
                    info = json.loads((out / 'BUILD.json').read_text())
                    self.assertEqual(info['tag'], 'menu-v1.2.3' if component == 'menu' else 'desktop-v3.4.5')
                    for line in (out / 'SHA256SUMS').read_text().splitlines():
                        digest, name = line.split()
                        self.assertEqual(digest, release.digest(out / name))
                    before = {p.name: p.read_bytes() for p in out.iterdir()}
                    with self.assertRaisesRegex(ValueError, 'already exists'):
                        release.stage(component, ['Linux-x64'], out, root)
                    self.assertEqual(before, {p.name: p.read_bytes() for p in out.iterdir()})

    def test_tampered_checksums_and_stale_menu_builds_leave_no_stage(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            out = root / 'output'
            (root / 'iso/boot.iso').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                release.stage('menu', [], out, root)
            self.assertFalse(out.exists())
            checksum = root / 'iso/boot.iso.sha256'
            checksum.write_text(release.digest(root / 'iso/boot.iso') + '  boot.iso\n')
            (root / 'src/main.c').write_text('#define MENU_VERSION "1.2.3"\n/* changed */\n')
            with self.assertRaisesRegex(ValueError, 'Rebuild menu'):
                release.stage('menu', [], out, root)
            self.assertFalse(out.exists())


if __name__ == '__main__':
    unittest.main()
