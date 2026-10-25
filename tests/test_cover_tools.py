#!/usr/bin/env python3
"""Exercise image conversion and catalog staging in temporary project roots."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import subprocess
import binascii
import tempfile
import zlib

ROOT = Path(__file__).resolve().parent.parent


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def rejects(function, *args):
    try:
        function(*args)
    except (ValueError, FileNotFoundError):
        return
    raise AssertionError("Expected invalid cover input to be rejected")


def main():
    prepare = module("prepare_cover_test", "prepare-cover.py")
    build = module("build_covers_test", "build-covers.py")
    export = module("export_covers_test", "export-covers.py")
    for path in (
        "../Game.iso",
        "Games/../Game.iso",
        "Games//Game.iso",
        "Game.txt",
        "Bad\nGame.iso",
    ):
        rejects(prepare.normalize, path)
    assert prepare.normalize("Games\\Disc 1.ISO") == "Games/Disc 1.ISO"
    assert build.c_string('A"é') == '"\\101\\042\\303\\251"'
    build.validate_cel(ROOT / "assets/demo-cover.cel")
    with tempfile.TemporaryDirectory(prefix="oh3do-cover-test-") as directory:
        temporary = Path(directory)
        (temporary / "assets").mkdir()
        shutil.copyfile(ROOT / "assets/demo-cover.cel", temporary / "assets/demo-cover.cel")
        prepare.ROOT = build.ROOT = temporary
        stage = temporary / "stage"
        (stage / "src").mkdir(parents=True)
        (stage / "takeme").mkdir()
        sys.argv = ["build-covers", str(stage), "menu"]
        build.main()
        assert "menu_cover_count = 0" in (stage / "src/cover_catalog.c").read_text()
        sys.argv = ["build-covers", str(stage), "demo"]
        build.main()
        assert (stage / "takeme/covers/demo.cel").is_file()
        if not shutil.which("3it") or not (shutil.which("convert") or shutil.which("magick")):
            raise SystemExit("Run this test through ./tools/dev-container run make test-covers")
        sys.argv = [
            "prepare-cover",
            'Games/Quoted "Disc".ISO',
            str(ROOT / "assets/loading-menu.png"),
        ]
        prepare.main()
        index = temporary / "assets/covers/index.json"
        entries = json.loads(index.read_text())
        filename = next(iter(entries.values()))
        cel = temporary / "assets/covers" / filename
        build.validate_cel(cel)
        sys.argv = [
            "prepare-cover",
            'GAMES/Quoted "Disc".iso',
            str(ROOT / "assets/loading-menu.png"),
        ]
        prepare.main()
        assert len(json.loads(index.read_text())) == 1  # case-only update
        sys.argv = ["build-covers", str(stage), "menu"]
        build.main()
        assert not (stage / "takeme/covers" / filename).exists()
        assert "menu_cover_count = 0" in (stage / "src/cover_catalog.c").read_text()
        usb = temporary / "iso/covers"
        binary_index = (usb / "index.3di").read_bytes()
        assert binary_index[:8] == b"OM3DIDX1"
        assert struct.unpack_from(">I", binary_index, 16)[0] == binascii.crc32(binary_index[24:])
        converted = list(usb.glob("*.3dc"))
        assert len(converted) == 1
        # The menu's prepared-cover decoder reads the exported file back exactly.
        cover = converted[0].read_bytes()
        assert cover[:8] == b"OM3DCOV2" and cover[-8:] == b"END3DC02"
        expected = temporary / "expected.rgb555"
        expected.write_bytes(zlib.decompress(cover[24:-8]))
        subprocess.run(
            [str(ROOT / "build/test_usb_media"), "fastcover", str(converted[0]), str(expected)],
            check=True,
        )
        # Validate rejection before malformed art can reach the cel engine.
        damaged = bytearray(cel.read_bytes())
        struct.pack_into(">I", damaged, 72, 320)
        bad = temporary / "bad.cel"
        bad.write_bytes(damaged)
        rejects(build.validate_cel, bad)
        bad.write_bytes(cel.read_bytes()[:-4])
        rejects(build.validate_cel, bad)
        index.write_text(json.dumps({"Game.iso": "../../bad.cel"}))
        rejects(export.export, temporary, usb)
        index.write_text(json.dumps({"Game.iso": filename, "GAME.ISO": filename}))
        rejects(export.export, temporary, usb)
    print("Cover tools: USB export, C decoder, demo staging, replacement and invalid inputs passed")


if __name__ == "__main__":
    main()
