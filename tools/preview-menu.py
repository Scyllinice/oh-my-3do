#!/usr/bin/env python3
"""Render the real draw() routine with host graphics stubs and SDK font pixels.

This checks layout only; it is not an emulator or a console screenshot.
Requires cc and the reference Portfolio checkout used by test-firmware.

How it works:
  1. The menu's drawing, loading, browsing and tools code is cut out of
     src/main.c and joined with the files in tools/menu-preview/:
       host-stubs.c    stand-ins for the 3DO graphics, font, sound and USB
                       calls, which print what they would draw
       nvram-stubs.c   stand-ins for the console's save memory
       self-tests.c    main(): a self-test of menu behavior, benchmarks, and
                       one "mode" per screen to render
     The joined program is saved as build/menu-preview.c and compiled with
     the menu's other source files.
  2. The self-tests and benchmarks run; any failed check stops the script.
  3. Each screen mode prints its drawing commands, which are checked and
     turned into build/menu-preview-MODE.svg (and a 960x720 .png when
     ImageMagick is installed), using the SDK's 8x8 font for text.
"""
import base64
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
STUB_FOLDER = ROOT / "tools/menu-preview"
OUTPUT = ROOT / "build"
SCREEN_WIDTH = 320
SCREEN_HEIGHT = 240

# The menu's own source files compiled alongside the generated program.
MENU_SOURCES = [
    "ode.c",
    "menu_sort.c",
    "menu_help.c",
    "game_info.c",
    "menu_ui.c",
    "menu_categories.c",
    "ui_font.c",
    "usb_art.c",
    "usb_file.c",
    "cover_cache.c",
    "usb_media.c",
    "preview_v5.c",
    "preview_stream.c",
    "png_decode.c",
    "png_inflate.c",
    "art_navigation.c",
    "save_archive.c",
    "save_cheats.c",
    "save_cheats_usb.c",
    "save_usb.c",
    "save_names.c",
    "save_catalog.c",
    "library_index.c",
    "library_features.c",
    "library_navigation.c",
    "menu_settings.c",
    "menu_config.c",
    "menu_position.c",
]

# Modes that run checks or benchmarks rather than rendering a screen.
CHECK_MODES = [
    "font-memory",
    "font-measure-bench",
    "tools-return",
    "idle-draw-bench",
    "fullscreen-render",
]

# Screens rendered to SVG, in order.
RENDER_MODES = [
    "context-help",
    "context-hex",
    "context-confirm",
    "library",
    "queue",
    "cover",
    "loading",
    "nested",
    "tools",
    "help",
    "diagnostics",
    "diagnostics-state",
    "diagnostics-checked",
    "saves",
    "save-usb",
    "save-hex",
    "restore",
    "display",
    "display-save",
    "large",
    "margin",
    "favorites",
    "slot-details",
    "find",
    "filtered",
    "unsaved",
    "recent",
    "alphabet",
    "alphabet-large",
    "tools-margin",
    "settings-margin",
    "results",
    "formats",
    "formats-large",
    "metadata",
    "metadata-large",
    "metadata-margin",
    "metadata-missing",
    "metadata-long",
    "gecko",
    "gecko-metadata",
    "gecko-cover",
    "gecko-preview",
    "gecko-folder",
    "gecko-tools",
    "gecko-settings",
    "gecko-large",
    "gecko-margin",
    "gecko-alphabet",
    "gecko-tuning",
    "gecko-queued",
    "gecko-folder-game",
    "gecko-full",
    "gecko-preview-full",
]

# Picture tags in "I" commands: 1 the logo, 2 a cover, 4 a preview frame.
IMAGE_ASSETS = {
    "1": ("assets/logo-menu.png", "image/png"),
    "2": ("assets/demo-cover.png", "image/png"),
    "4": ("assets/demo-preview.svg", "image/svg+xml"),
}


def color(value):
    """An RGB555 color number as #rrggbb."""
    value = int(value)
    return "#" + "".join(f"{((value >> shift) & 31) * 255 // 31:02x}" for shift in (10, 5, 0))


# Building the program


def between(source, start, end):
    """The part of `source` from the text `start` up to the text `end`."""
    return source[source.index(start) : source.index(end)]


def generated_program():
    """The preview program's source: the stubs and slices of src/main.c,
    with #defines redirecting the slices' USB calls to the stubs."""
    source = (ROOT / "src/main.c").read_text()
    drawing = between(source, "#define MENU_VERSION", "static void progress(")
    loading = between(source, "static void progress(", "static int directory_path(")
    covers = between(source, "static int directory_path(", "static void show_demo(")
    demo = between(source, "static void show_demo(", "static void connect_ode(")
    back = between(source, "static void go_back(Menu *menu)\n{", "static void activate(Menu *menu)")
    queue = between(
        source, "static void queue_selected_disc(Menu *menu)\n{", "/* ==== Applying the disc queue"
    )
    tools = between(
        source, "/* ==== Applying the disc queue", "/* ==== Starting the menu and the main loop"
    )

    text_files = "\n#define usb_file_text preview_text_read\n#define usb_file_text_write preview_text_write\n"
    loading_stubs = (
        "\n#define ode_read_directory preview_read_directory\n"
        "\n#define usb_art_index_load preview_art_index\n"
        "#define usb_media_index_load preview_video_index\n"
        "#define library_metadata_load preview_metadata_index\n"
    )
    cover_stubs = (
        "\n#undef usb_art_index_load\n#undef usb_media_index_load\n"
        "#undef library_metadata_load\n#undef ode_read_directory\n"
        "#define usb_preview_load_reclaim preview_video_load_profiled\n"
        "#define usb_png_cover preview_png_load\n"
    )
    end_cover_stubs = "\n#undef usb_preview_load_reclaim\n#undef usb_png_cover\n"

    return (
        (STUB_FOLDER / "host-stubs.c").read_text()
        + text_files
        + drawing
        + loading_stubs
        + loading
        + cover_stubs
        + covers
        + end_cover_stubs
        + demo
        + back
        + (STUB_FOLDER / "nvram-stubs.c").read_text()
        + text_files
        + queue
        + tools
        + (STUB_FOLDER / "self-tests.c").read_text()
    )


# The OM3DVID7 preview codec library.
LIBRARY_SOURCES = ["om3dvid/src/om3dvid7_decoder.c"]


def compile_program(code, binary):
    command = [os.environ.get("CC", "cc"), "-std=c99", "-O1", "-I", str(ROOT / "src")]
    command += ["-I", str(ROOT / "om3dvid/include"), str(code)]
    command += [str(ROOT / "src" / name) for name in MENU_SOURCES]
    command += [str(ROOT / name) for name in LIBRARY_SOURCES]
    command += ["-o", str(binary)]
    subprocess.run(command, check=True)


def run_mode(binary, mode):
    """The program's output for one mode, as lines."""
    return subprocess.check_output([str(binary), mode], text=True).splitlines()


# The checks


def run_self_tests(binary):
    subprocess.run([str(binary), "self-test"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([str(binary), "browser-render"], check=True, stdout=subprocess.DEVNULL)
    print(
        "Menu view/input: preview scheduling/playback/cancellation, alphabet focus/jump/cancel, "
        "browser, configuration discard, find keyboard, history persistence, favorite ordering "
        "and diagnostics export passed"
    )
    for mode in CHECK_MODES:
        subprocess.run([str(binary), mode], stdout=subprocess.DEVNULL, check=True)

    # Each "FRAME" is one drawn frame; count the logos ("I 1") in each.
    playback = subprocess.check_output([str(binary), "gecko-playback"], text=True)
    frames = playback.split("FRAME ")[1:]
    logos = [sum(line.startswith("I 1 ") for line in frame.splitlines()) for frame in frames]
    # Light frames leave the list panel alone; a new selection redraws it.
    assert logos == [1, 1, 0, 0, 1], logos
    print("Gecko playback: picture-only frames once both buffers hold the list panel")

    timing = run_mode(binary, "diagnostics-preview")
    assert "L Last preview load" in timing and "L Read + CRC: 24100ms" in timing
    assert "L Folder navigation: 7100ms" in timing and "L Frames: 225 at 15 fps" in timing

    file_lines = run_mode(binary, "diagnostics-file")
    assert "L Last requested USB file:" in file_lines
    assert "L Indexes: 536 covers / 282 clips" in file_lines
    assert "L Game metadata: 483 entries" in file_lines
    assert "L Retained after a USB timeout." in file_lines


def count_starting(commands, prefix):
    return sum(command.startswith(prefix) for command in commands)


def check_screen(mode, commands):
    """Mode-specific checks of a rendered screen's commands ("L" lines are
    the text drawn, "I" lines pictures)."""
    if mode == "gecko-cover":
        assert (
            count_starting(commands, "I 2 ") == 1
        ), "Draw one centered cover without a stretched duplicate"
    if mode.startswith("gecko") and mode not in ("gecko-full", "gecko-preview-full"):
        assert count_starting(commands, "I 1 ") == 1, "Gecko TV must retain the graphical logo"
    if mode == "gecko-folder-game":
        assert "L Adventure  -  1994" in commands and "L Play" in commands
        assert "L Browse folder" not in commands
    if mode == "gecko-queued":
        assert "L QUEUE 2" in commands and "L Queue" in commands  # OSD readout and the P prompt
    if mode == "gecko-preview-full":
        assert "I 4 0.000000 0.000000 2.000000 2.000000" in commands
    if mode == "gecko-preview":
        assert "L Cover" in commands  # X swaps the playing preview for the cover
        # The whole frame at native size, beside the solid list.
        assert "I 4 152.000000 45.000000 1.000000 1.000000" in commands
    if mode == "gecko-settings":
        assert "L Folder workaround" in commands and "L AUTO" in commands
        assert not any("Theme" in command for command in commands)
    if mode in ("metadata", "metadata-large", "metadata-margin"):
        assert "L Adventure  -  1994" in commands and "L Infogrames" in commands
    if mode == "metadata-missing":
        assert "L No game information" in commands
    if mode == "results":
        assert "L ISO" in commands and "L CUE" in commands
    if mode in ("tools", "diagnostics-state"):
        assert not any("USB write" in command for command in commands)
    if mode == "diagnostics-checked":
        assert any("USB write check: PASS" in command for command in commands)


# Turning drawing commands into SVG


def check_inside_screen(x, y, width, height):
    assert 0 <= x < x + width <= SCREEN_WIDTH
    assert 0 <= y < y + height <= SCREEN_HEIGHT


def svg_rect(x, y, width, height, fill, extra=""):
    return f'<rect x="{x}" y="{y}" width="{width}" height="{height}" fill="{fill}"{extra}/>'


def bitmap_text(fields):
    """B x y sx sy palette width height data: a 4-bit coverage bitmap (the
    UI font's text), each value 1-15 a palette color. Rows are 144 bytes."""
    _, x, y, sx, sy, palette, width, height, data = fields
    x, y, sx, sy = map(float, (x, y, sx, sy))
    data = bytes.fromhex(data)
    palette = palette.split(",")
    # Text is drawn in screen pixels; the mascot uses the
    # menu's margin transform like the other artwork cels.
    assert 0 < sx <= 1 and 0 < sy <= 1
    shapes = []
    for row in range(int(height)):
        for col in range(int(width)):
            coverage = (data[row * 144 + col // 2] >> (0 if col % 2 else 4)) & 15
            if coverage:
                assert 0 <= x + col * sx < SCREEN_WIDTH and 0 <= y + row * sy < SCREEN_HEIGHT
                shapes.append(
                    svg_rect(x + col * sx, y + row * sy, sx, sy, color(palette[coverage]))
                )
    return shapes


def pixel_picture(fields):
    """P x y sx sy width height data: RGB555 pixels, 4 hex digits each;
    a negative sy draws the rows upward. Zero pixels are transparent."""
    _, x, y, sx, sy, width, height, data = fields
    x, y, sx, sy = map(float, (x, y, sx, sy))
    width, height = int(width), int(height)
    shapes = []
    for row in range(height):
        top = y + row * sy if sy > 0 else y + row * sy + sy + 1
        for col in range(width):
            start = (row * width + col) * 4
            value = int(data[start : start + 4], 16)
            left = x + col * sx
            if value and -abs(sx) < left < SCREEN_WIDTH and -abs(sy) < top < SCREEN_HEIGHT:
                shapes.append(svg_rect(left, top, sx, abs(sy), color(str(value))))
    return shapes


def image_size(tag, logo_size):
    if tag == "4":
        return 160, 120
    if tag == "2":
        return 64, 80
    return logo_size


def picture(fields, logo_size):
    """I tag x y sx sy: a picture (see IMAGE_ASSETS), scaled."""
    _, tag, x, y, sx, sy = fields
    x, y, sx, sy = map(float, (x, y, sx, sy))
    path, media_type = IMAGE_ASSETS.get(tag, IMAGE_ASSETS["1"])
    art = base64.b64encode((ROOT / path).read_bytes()).decode()
    width, height = image_size(tag, logo_size)
    return [
        f'<image x="{x}" y="{y}" width="{width*sx}" height="{height*sy}" '
        f'xlink:href="data:{media_type};base64,{art}"/>'
    ]


def glass(fields):
    """G x y w h color keep: a tinted panel keeping keep/8 of what is under it."""
    _, x, y, w, h, c, keep = fields
    check_inside_screen(int(x), int(y), int(w), int(h))
    return [svg_rect(x, y, w, h, color(c), f' fill-opacity="{(8-int(keep))/8}"')]


def solid(fields):
    """R x y w h color: a filled rectangle."""
    _, x, y, w, h, c = fields
    check_inside_screen(int(x), int(y), int(w), int(h))
    return [svg_rect(x, y, w, h, color(c))]


def sdk_text(command, font):
    """T x y color sx sy label: text in the SDK's 8x8 font."""
    glyphs, images = font
    _, x, y, c, sx, sy, label = command.split(" ", 6)
    x, y = float(x), float(y)
    sx, sy = float(sx), float(sy)
    assert x >= 0 and x + (len(label) - 1) * 8 + 8 * sx <= 320.01, label
    assert y >= 0 and y + 8 * sy <= 240.01, label
    shapes = []
    for char in label.upper():
        for row, bits in enumerate(glyphs.get(char, images["Image_WhatTheHell"])):
            for col in range(8):
                if bits & (128 >> col):
                    shapes.append(
                        f'<rect x="{x+col*sx}" y="{y+row*sy}" width="{sx}" '
                        f'height="{sy}" fill="{color(c)}"/>'
                    )
        x += 8
    return shapes


def commands_to_svg(commands, font, logo_size):
    svg = [
        '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        'width="320" height="240" viewBox="0 0 320 240" shape-rendering="crispEdges">'
    ]
    for command in commands:
        kind = command[:2]
        if kind == "L ":
            continue  # the text's content, already drawn by another command
        if kind == "B ":
            svg += bitmap_text(command.split())
        elif kind == "P ":
            svg += pixel_picture(command.split())
        elif kind == "I ":
            svg += picture(command.split(), logo_size)
        elif kind == "G ":
            svg += glass(command.split())
        elif kind == "R ":
            svg += solid(command.split())
        else:
            svg += sdk_text(command, font)
    svg.append("</svg>")
    return "\n".join(svg)


def load_sdk_font():
    """The SDK's 8x8 font, from the Portfolio OS source: ({character: rows},
    {image name: rows}), each row a byte of pixel bits."""
    font = (ROOT / "third_party/portfolio_os/src/graphics/text_deffont.c").read_text()
    images = {}
    for name, body in re.findall(r"CelData (Image_\w+)\[\]\s*=\s*\{(.*?)\};", font, re.S):
        words = re.findall(r"0x([0-9a-fA-F]{8})", body)
        assert len(words) == 24, name
        images[name] = [int(word, 16) >> 24 for word in words[::3]]
    glyphs = {
        char: images[name] for char, name in re.findall(r"\(int32\)'(.)', 8, (Image_\w+)", font)
    }
    return glyphs, images


def write_png(svg_path, directory):
    """Convert an SVG to a 960x720 PNG with ImageMagick, if it is installed."""
    if not shutil.which("magick"):
        return
    # ImageMagick's internal SVG reader cannot resolve data URLs.
    # Use an absolute asset path in its temporary input only.
    text = svg_path.read_text()
    for path, media_type in IMAGE_ASSETS.values():
        encoded = base64.b64encode((ROOT / path).read_bytes()).decode()
        text = text.replace(f"data:{media_type};base64,{encoded}", str(ROOT / path))
    local = Path(directory) / "render.svg"
    local.write_text(text)
    png = svg_path.with_suffix(".png")
    subprocess.run(
        ["magick", str(local), "-filter", "point", "-resize", "960x720", str(png)], check=True
    )
    print(png)


def render_screens(binary, directory):
    font = load_sdk_font()
    logo_size = struct.unpack_from(">II", (ROOT / "assets/logo-menu.png").read_bytes(), 16)
    for mode in RENDER_MODES:
        commands = run_mode(binary, mode)
        check_screen(mode, commands)
        target = OUTPUT / f"menu-preview-{mode}.svg"
        target.write_text(commands_to_svg(commands, font, logo_size))
        print(target)
        write_png(target, directory)


def main():
    OUTPUT.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="oh3do-preview-") as directory:
        code = Path(directory) / "preview.c"
        binary = Path(directory) / "preview"
        code.write_text(generated_program())
        (OUTPUT / "menu-preview.c").write_text(code.read_text())
        compile_program(code, binary)
        run_self_tests(binary)
        render_screens(binary, directory)


if __name__ == "__main__":
    main()
