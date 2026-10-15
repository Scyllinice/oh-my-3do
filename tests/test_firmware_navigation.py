#!/usr/bin/env python3
"""Reproduce the ID collision using the upstream firmware function itself."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "third_party/SataTo3DO/src/interfaces/MSC.c"
PATCH = ROOT / "firmware/0001-relative-folder-ids.patch"


def extract(source):
    start = source.index("void setTocLevel(int index) {")
    end = source.index("\nstatic bool getNextValidToc", start)
    return source[start:end]


def run_fixture(directory, source, name):
    harness = (ROOT / "tests/firmware_navigation_harness.c").read_text()
    code = directory / f"{name}.c"
    binary = directory / name
    code.write_text(harness.replace("/* INSERT_FIRMWARE_SET_TOC_LEVEL */", extract(source)))
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c99", "-O1", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
        "-I", str(ROOT / "src"), str(code), str(ROOT / "src/ode.c"),
        str(ROOT / "src/art_navigation.c"), str(ROOT / "src/library_navigation.c"),
        str(ROOT / "src/menu_ui.c"), str(ROOT / "src/cover_catalog.c"),
        str(ROOT / "src/menu_sort.c"), str(ROOT / "src/library_features.c"), "-o", str(binary),
    ], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    if not SOURCE.is_file():
        raise SystemExit("Check out FCare/SataTo3DO in third_party/SataTo3DO before running this test.")
    with tempfile.TemporaryDirectory(prefix="oh3do-firmware-nav-") as temporary:
        directory = Path(temporary)
        original = SOURCE.read_text()
        before = run_fixture(directory, original, "original")
        print("Unmodified firmware:")
        print(before.stdout, end="")
        assert before.returncode != 0
        assert "FAIL stale-root-ID" in before.stdout
        assert "FAIL repeated-child-ID" in before.stdout
        staged = directory / "src/interfaces/MSC.c"
        staged.parent.mkdir(parents=True)
        staged.write_text(original)
        subprocess.run(["patch", "--batch", "--forward", "-p1", "-i", str(PATCH)],
                       cwd=directory, check=True, capture_output=True, text=True)
        after = run_fixture(directory, staged.read_text(), "patched")
        print("Patched firmware:")
        print(after.stdout, end="")
        assert after.returncode == 0, after.stderr
        print("Firmware navigation regression: reproduced before fix; all cases pass after fix")


if __name__ == "__main__":
    main()
