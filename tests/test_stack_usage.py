#!/usr/bin/env python3
"""Bound actual ARM stack allocations on the 32 KiB menu launch path."""
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ["TDO_DEVKIT_PATH"])
out = root / "build/stack-audit"
out.mkdir(parents=True, exist_ok=True)
assembly = out / "main.s"
result = subprocess.run(
    [
        "armcc",
        "-I" + str(sdk / "include/3do"),
        "-I" + str(sdk / "include/community"),
        "-DOH3DO_SDK=1",
        "-DNDEBUG=1",
        "-O2",
        "-bigend",
        "-za1",
        "-zi4",
        "-fpu",
        "none",
        "-arch",
        "3",
        "-apcs",
        "3/32/nofp/swst/wide/softfp",
        "-S",
        str(root / "src/main.c"),
        "-o",
        str(assembly),
    ],
    capture_output=True,
    text=True,
)
(out / "compile.log").write_text(result.stdout + result.stderr)
result.check_returncode()
text = assembly.read_text()
# Saved registers add at most 64 bytes per function. Prevent activate's frame
# from overlapping queue_set's large buffers on a single call chain.
limits = {
    "draw_game_details": 512,
    "game_details_text": 256,
    "search_library": 512,
    "apply_queue": 1024,
    "activate": 512,
    "queue_set": 20000,
    "remember_position": 512,
    "record_launch": 512,
    "export_diagnostics": 8192,
    "update_cover": 13000,
    "update_preview": 1024,
    "tools_input": 4096,
}
summary = []
for name, limit in limits.items():
    body = text.split("\n" + name + "\n", 1)[1]
    # Norcroft reserves locals before the first ordinary call (BLCC is its
    # stack-limit guard). Inspect generated instructions, not C declarations.
    prologue = re.split(r"\n\s+BL\s+", body, maxsplit=1)[0]
    size = sum(
        int(n, 0) for n in re.findall(r"^\s+SUB\s+sp,sp,#(0x[0-9a-fA-F]+|\d+)\s*$", prologue, re.M)
    )
    assert size <= limit, f"{name}: {size} bytes exceeds {limit}-byte local-stack budget"
    summary.append(f"{name}: {size} bytes of explicit local stack")
(out / "frames.txt").write_text("\n".join(summary) + "\n")
print("ARM stack: small launch/persistence frames; disc grouping and diagnostics fit their budgets")

# Bound the combined cover -> streaming PNG/USB/inflate path as well.
frames = {}
for source, names in {
    "main.c": ["update_cover", "update_preview", "read_usb_preview", "preview_phase"],
    "library_navigation.c": ["library_catalog_build"],
    "menu_categories.c": ["menu_category_toggle", "menu_categories_parse"],
    "usb_media.c": [
        "usb_png_cover",
        "png_pixels",
        "smooth_canvas",
        "usb_preview_load",
        "usb_preview_load_profiled",
        "usb_preview_load_options",
        "preview_stream",
        "preview_store",
        "preview_block",
        "usb_fast_cover",
        "usb_preview_frame",
    ],
    "usb_art.c": ["usb_file_read"],
    "png_decode.c": ["pngle_feed"],
    "png_inflate.c": ["tinfl_decompress"],
}.items():
    target = out / (source + ".s")
    command = [
        "armcc",
        "-I" + str(sdk / "include/3do"),
        "-I" + str(sdk / "include/community"),
        "-DOH3DO_SDK=1",
        "-DNDEBUG=1",
        "-O2",
        "-bigend",
        "-za1",
        "-zi4",
        "-fpu",
        "none",
        "-arch",
        "3",
        "-apcs",
        "3/32/nofp/swst/wide/softfp",
        "-S",
        str(root / "src" / source),
        "-o",
        str(target),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (out / (source + ".log")).write_text(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    for name in names:
        body = target.read_text().split("\n" + name + "\n", 1)[1]
        prologue = re.split(r"\n\s+BL\s+", body, maxsplit=1)[0]
        frames[name] = sum(
            int(n, 0)
            for n in re.findall(r"^\s+SUB\s+sp,sp,#(0x[0-9a-fA-F]+|\d+)\s*$", prologue, re.M)
        )
        assert frames[name] < 14000, (name, frames[name])
# Includes both USB and inflate frames, although those calls run separately;
# reserve another 4 KiB for saved registers and intermediate library calls.
assert sum(frames.values()) + 4096 < 32768, frames
(out / "media-frames.txt").write_text(
    "\n".join(f"{name}: {size}" for name, size in frames.items()) + "\n"
)
print("ARM media stack: conservative PNG/USB/inflate frame sum fits 32 KiB")

assert (
    frames["menu_category_toggle"] < 512 and frames["menu_categories_parse"] < 512
), "Category JSON must stay off the small console stack"
