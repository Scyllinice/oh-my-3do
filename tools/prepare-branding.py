#!/usr/bin/env python3
"""Export approved logo sources into console and desktop asset formats.

Usage: prepare-branding.py [--console] [--windows-resource]

From assets/logo-source.png and assets/app-icon-source.png, writes with
ImageMagick:
  logo-menu.png      the menu's 128x34 wordmark
  logo-desktop.png   the desktop app's 768x240 wordmark
  app-icon.png/.ico  the desktop app icon (the .ico holds sizes 16 to 256)
and paints the new wordmark into startup-menu.png and loading-menu.png.

--console also converts those images into the 3DO formats the menu loads
(logo-menu.cel, loading.banner, startup.cel) with the SDK container.
--windows-resource also compiles app-icon.rc into app-icon.res for the
Windows build, inside the desktop cross-build container.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"

# Each wordmark export: (file name, size to fit inside).
WORDMARKS = [("logo-menu.png", "128x34"), ("logo-desktop.png", "768x240")]

ICON_SIZES = "256,128,64,48,32,24,16"

# The startup and loading screens share a layout: a logo panel, filled with
# this color, inside which the wordmark is placed.
SPLASH_SCREENS = ["startup-menu.png", "loading-menu.png"]
SPLASH_PANEL_COLOR = "#080810"
SPLASH_PANEL_RECTANGLE = "rectangle 56,66 263,144"
SPLASH_LOGO_SIZE = "196x70"
SPLASH_LOGO_POSITION = "+0+76"  # centered, 76 pixels from the top

# Pure black is the 3DO's transparent color in these images, so raise every
# channel of the logo to at least 8/255 (3.137255%) to keep it opaque.
SPLASH_LOGO_MINIMUM_LEVEL = "3.137255%"


def run(*command):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, check=True)


def export_desktop_images(magick):
    """Write the wordmarks and app icons from their source images."""
    source = ASSETS / "logo-source.png"
    for name, size in WORDMARKS:
        run(magick, source, "-trim", "+repage", "-resize", size, ASSETS / name)

    icon_source = ASSETS / "app-icon-source.png"
    run(magick, icon_source, "-resize", "256x256", ASSETS / "app-icon.png")
    run(magick, icon_source, "-define", "icon:auto-resize=" + ICON_SIZES, ASSETS / "app-icon.ico")


def update_splash_screens(magick):
    """Paint the new wordmark into each splash screen, in place.

    Keeps the startup screen's native layout and exact black transparency
    mask: only the logo panel is cleared and redrawn.
    """
    source = ASSETS / "logo-source.png"
    for name in SPLASH_SCREENS:
        path = ASSETS / name
        run(
            magick,
            path,
            # Clear the old logo.
            "-fill",
            SPLASH_PANEL_COLOR,
            "-draw",
            SPLASH_PANEL_RECTANGLE,
            # Prepare the new one: trimmed, resized and kept off pure black.
            "(",
            source,
            "-trim",
            "+repage",
            "-resize",
            SPLASH_LOGO_SIZE,
            "-channel",
            "RGB",
            "-evaluate",
            "Max",
            SPLASH_LOGO_MINIMUM_LEVEL,
            "+channel",
            ")",
            # Draw it over the panel.
            "-gravity",
            "north",
            "-geometry",
            SPLASH_LOGO_POSITION,
            "-compose",
            "over",
            "-composite",
            path,
        )


def export_console_images():
    """Convert the menu's images to 3DO cels and a boot banner with the SDK's
    3it tool, run inside the dev container (which sees the project at
    /workspace)."""
    dev = ROOT / "tools/dev-container"

    # The menu logo: packed, with black as transparent.
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

    # The banner the console shows while the menu loads.
    run(
        dev,
        "run",
        "3it",
        "to-banner",
        "-o",
        "/workspace/assets/loading.banner",
        "/workspace/assets/loading-menu.png",
    )

    # The full-screen startup image: unpacked and opaque.
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


def compile_windows_resource():
    """Compile app-icon.rc with llvm-rc in the Windows cross-build image,
    running as the current user so the output isn't owned by root."""
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

    export_desktop_images(magick)
    update_splash_screens(magick)
    if args.console:
        export_console_images()
    if args.windows_resource:
        compile_windows_resource()


if __name__ == "__main__":
    main()
