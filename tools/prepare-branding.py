#!/usr/bin/env python3
"""Export approved logo sources into console and desktop asset formats."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"


def run(*command):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--console",
        action="store_true",
        help="also regenerate CELs and the boot banner with the SDK container",
    )
    parser.add_argument(
        "--windows-resource",
        action="store_true",
        help="also compile the Windows icon resource using the desktop cross-build container",
    )
    args = parser.parse_args()
    magick = shutil.which("magick")
    if not magick:
        parser.error("Install ImageMagick (magick) to export branding assets")
    source = ASSETS / "logo-source.png"
    for name, size in [("logo-menu.png", "128x34"), ("logo-desktop.png", "768x240")]:
        run(magick, source, "-trim", "+repage", "-resize", size, ASSETS / name)
    run(magick, ASSETS / "app-icon-source.png", "-resize", "256x256", ASSETS / "app-icon.png")
    run(
        magick,
        ASSETS / "app-icon-source.png",
        "-define",
        "icon:auto-resize=256,128,64,48,32,24,16",
        ASSETS / "app-icon.ico",
    )
    # Keep the startup screen's native layout and exact black transparency
    # mask. Replace only its logo panel with the newly exported wordmark.
    for name in ["startup-menu.png", "loading-menu.png"]:
        path = ASSETS / name
        run(
            magick,
            path,
            "-fill",
            "#080810",
            "-draw",
            "rectangle 56,66 263,144",
            "(",
            source,
            "-trim",
            "+repage",
            "-resize",
            "196x70",
            "-channel",
            "RGB",
            "-evaluate",
            "Max",
            "3.137255%",
            "+channel",
            ")",
            "-gravity",
            "north",
            "-geometry",
            "+0+76",
            "-compose",
            "over",
            "-composite",
            path,
        )
    if args.console:
        dev = ROOT / "tools/dev-container"
        run(
            dev,
            "run",
            "3it",
            "to-cel",
            "--bpp",
            "16",
            "--packed",
            "true",
            "--transparent",
            "0x00000000",
            "--ccb-last",
            "set",
            "-o",
            "/workspace/assets/logo-menu.cel",
            "/workspace/assets/logo-menu.png",
        )
        run(
            dev,
            "run",
            "3it",
            "to-banner",
            "-o",
            "/workspace/assets/loading.banner",
            "/workspace/assets/loading-menu.png",
        )
        run(
            dev,
            "run",
            "3it",
            "to-cel",
            "--bpp",
            "16",
            "--packed",
            "false",
            "--ccb-last",
            "set",
            "--ccb-bgnd",
            "unset",
            "-o",
            "/workspace/assets/startup.cel",
            "/workspace/assets/startup-menu.png",
        )
    if args.windows_resource:
        run(
            "docker",
            "run",
            "--rm",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "-v",
            f"{ROOT}:/src",
            "-w",
            "/src/assets",
            "oh3do-desktop-windows",
            "llvm-rc",
            "/fo",
            "app-icon.res",
            "app-icon.rc",
        )


if __name__ == "__main__":
    main()
