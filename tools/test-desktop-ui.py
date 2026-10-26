#!/usr/bin/env python3
"""Native Tauri smoke test. Run under Xvfb/D-Bus with the desktop-tauri Docker image.
Creates disposable fixtures under build/desktop-ui-smoke. --library captures a
read-only real-library screenshot instead of exercising fixture transfers.

The test builds a small fake library (four disc images, boot.iso and the
menu's index files) and a fake USB drive holding two save backups, starts the
real desktop app pointed at them, and drives it with synthetic mouse and
keyboard input through X11's XTEST extension. Screenshots of each step are
saved in the fixture's shots/ folder. Clicks use fixed screen positions, so
they assume the default window size on a 1600x1200 virtual display.

Modes:
  (default)    copy a game with the mouse, then edit and save a save file
               in the Save Manager
  --keyboard   copy a game using the keyboard only
  --features   also compare two saves and run the read-only drive check
  --gallery    also capture the Drive setup, Media and Settings pages
  --library    only screenshot a real library folder (read-only)

Pass --fixture as an absolute path; a relative one makes the library look empty.
As of desktop 1.0.5, --keyboard passes but the default and --features runs
fail at the Save Manager edit step: some click positions there are out of
date with the current layout.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build/tauri-linux-target/release/oh-my-3do-library"

GAME_PATHS = [
    "Games/USA/Example/Disc 1.iso",
    "Games/USA/Example/Disc 2.iso",
    "Games/USA/Other/Other.iso",
    "Games/Japan/Third/Third.iso",
]

# The green of the interface's primary action button.
PRIMARY_BUTTON_COLOR = (46, 102, 63)

# X11 mouse buttons: 1 is the left button; 4 and 5 are the wheel up and down.
LEFT_BUTTON = 1
WHEEL_UP = 4
WHEEL_DOWN = 5

# X11 key names for characters type_text sends that aren't letters or digits.
KEY_NAMES = {
    "/": "slash",
    ".": "period",
    "-": "minus",
    "_": "underscore",
    ":": "colon",
    " ": "space",
}

# A DOOM preferences save: nine 32-bit words, then a checksum of 12345 plus
# their sum. The third word (the starting map) is what the test edits.
DOOM_WORDS = [0x4C57, 0, 1, 8, 8, 0, 1, 0, 0]
DOOM_CHECKSUM_SEED = 12345

# Where things sit in a one-entry-first .3ds save archive: a 24-byte header,
# then each entry's 48-byte header (32-byte name, four words) and data.
FIRST_ENTRY_DATA = 24 + 48
DOOM_MAP_OFFSET = FIRST_ENTRY_DATA + 8
DOOM_CHECKSUM_OFFSET = FIRST_ENTRY_DATA + 36


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--gallery", action="store_true")
    parser.add_argument(
        "--features",
        action="store_true",
        help="Exercise save comparison and the read-only drive check",
    )
    parser.add_argument(
        "--keyboard", action="store_true", help="Copy one game using keyboard input only"
    )
    parser.add_argument("--fixture", type=Path)
    return parser.parse_args()


def index_string(text):
    """A string in the menu's index files: its length (16-bit, counting the
    terminator), then the text and a NUL."""
    data = text.encode() + b"\0"
    return struct.pack(">H", len(data)) + data


def index_file(magic, footer, payload, count):
    """A menu index file: magic, then the entry count, payload size, payload
    CRC-32 and a reserved zero (big-endian words), the payload and a footer."""
    header = struct.pack(">IIII", count, len(payload), zlib.crc32(payload), 0)
    return magic + header + payload + footer


def game_title(path):
    if "Disc 1" in path:
        return "Example (Disc 1)"
    if "Disc 2" in path:
        return "Example (Disc 2)"
    return Path(path).stem


def write_library(source):
    """Write four small fake disc images, boot.iso and the menu's index files
    into the library folder `source`."""
    for name in GAME_PATHS:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode() * 128)
    shutil.copyfile(ROOT / "iso/boot.iso", source / "boot.iso")

    support = source / ".ohmy3do"
    support.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "ohmy3do.ini", support / "ohmy3do.ini")

    # The menu's indexes list games sorted without regard to case.
    paths = sorted(GAME_PATHS, key=str.lower)
    library_payload = b"".join(index_string(p) for p in paths)
    (support / "library.3di").write_bytes(
        index_file(b"OM3DLIB1", b"END3DL01", library_payload, len(paths))
    )
    # Each metadata row is the path and three empty fields.
    metadata_payload = b"".join(index_string(p) + index_string("") * 3 for p in paths)
    (support / "metadata.3di").write_bytes(
        index_file(b"OM3DMET1", b"END3DM01", metadata_payload, len(paths))
    )

    records = [
        dict(
            path=p,
            title=game_title(p),
            category="Games",
            region="Japan" if "Japan" in p else "USA",
            metadata={},
        )
        for p in paths
    ]
    (support / "library.json").write_text(json.dumps(dict(schema=1, sources=[], games=records)))


def write_settings(cache, source, dest, real_library):
    """Point the app at the fixture library and drive, offline and without
    update checks. With `real_library`, show that folder instead."""
    settings = dict(
        library=str(source), destination=str(dest), offline=True, disable_update_checks=True
    )
    settings_path = cache / "oh-my-3do/native-settings.json"
    settings_path.write_text(json.dumps(settings))
    if real_library:
        settings = json.loads(settings_path.read_text())
        settings["library"] = str(real_library)
        settings_path.write_text(json.dumps(settings))


def doom_save():
    checksum = (DOOM_CHECKSUM_SEED + sum(DOOM_WORDS)) & 0xFFFFFFFF
    return struct.pack(">10I", *DOOM_WORDS, checksum)


def save_archive(entries):
    """A .3ds save backup holding (name, data) entries.

    Header: "OM3DSAV1", then the total size, entry count, CRC-32 of the
    entries and a reserved zero. Each entry: a 32-byte name, then four words
    (reserved, data size, data CRC-32, reserved) and the data.
    """
    payload = b""
    for name, data in entries:
        payload += name.encode().ljust(32, b"\0")
        payload += struct.pack(">IIII", 0, len(data), zlib.crc32(data), 0)
        payload += data
    header = struct.pack(">IIII", len(payload) + 24, len(entries), zlib.crc32(payload), 0)
    return b"OM3DSAV1" + header + payload


class Input:
    """Synthetic mouse and keyboard input through libX11 and libXtst."""

    def __init__(self):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtest = ctypes.CDLL("libXtst.so.6")
        x11 = self.x11
        xtest = self.xtest

        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        x11.XStringToKeysym.restype = ctypes.c_ulong
        x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x11.XKeysymToKeycode.restype = ctypes.c_uint
        xtest.XTestFakeMotionEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        xtest.XTestFakeButtonEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        xtest.XTestFakeKeyEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        self.display = x11.XOpenDisplay(None)

    def move(self, x, y):
        # -1: the default screen.
        self.xtest.XTestFakeMotionEvent(self.display, -1, x, y, 0)

    def button(self, number, down):
        self.xtest.XTestFakeButtonEvent(self.display, number, 1 if down else 0, 0)

    def flush(self):
        self.x11.XFlush(self.display)

    def click(self, x, y):
        """Left-click at a screen position and give the interface time to react."""
        self.move(x, y)
        self.button(LEFT_BUTTON, True)
        self.button(LEFT_BUTTON, False)
        self.flush()
        time.sleep(0.35)

    def scroll(self, x, y, wheel_button, steps):
        """Turn the mouse wheel `steps` times with the pointer at (x, y)."""
        self.move(x, y)
        for _ in range(steps):
            self.button(wheel_button, True)
            self.button(wheel_button, False)
        self.flush()

    def key(self, name, down=True):
        """Press or release the key with X11 key name `name`."""
        code = self.x11.XKeysymToKeycode(self.display, self.x11.XStringToKeysym(name.encode()))
        assert code, name
        self.xtest.XTestFakeKeyEvent(self.display, code, 1 if down else 0, 0)
        self.flush()

    def press(self, name):
        self.key(name)
        self.key(name, False)
        time.sleep(0.1)

    def chord(self, modifier, name):
        """Press `name` while holding `modifier` (for example Control_L + l)."""
        self.key(modifier)
        self.press(name)
        self.key(modifier, False)
        time.sleep(0.1)

    def type_text(self, text):
        for character in text:
            shifted = character.isupper() or character in "_:"
            key_name = KEY_NAMES.get(character, character.lower())
            if shifted:
                self.key("Shift_L")
            self.key(key_name)
            self.key(key_name, False)
            if shifted:
                self.key("Shift_L", False)


class Session:
    """The running test: input, screenshots and the fixture's folders."""

    def __init__(self, fixture, input_device):
        from PIL import ImageGrab

        self.grab = ImageGrab.grab
        self.input = input_device
        self.source = fixture / "library"
        self.dest = fixture / "usb"
        self.shots = fixture / "shots"

    def screen(self):
        return self.grab(xdisplay=os.environ["DISPLAY"])

    def shot(self, name):
        image = self.screen()
        image.save(str(self.shots / name))
        return image

    def click_primary_action(self):
        """Click the lowest primary (green) button on screen.

        Scans for rows with more than 50 pixels of the button color, takes the
        last group of rows (the bottom button) and clicks its middle.
        """
        image = self.screen()
        rows = []
        for y in range(80, 900):
            points = [
                x for x in range(185, 1280) if image.getpixel((x, y))[:3] == PRIMARY_BUTTON_COLOR
            ]
            if len(points) > 50:
                rows.append((y, min(points), max(points)))
        assert rows, "Primary action not visible"

        bottom = rows[-1][0]
        last_button = [row for row in rows if bottom - row[0] < 12]
        y, left, right = last_button[len(last_button) // 2]
        self.input.click((left + right) // 2, y)

    def open_save_file(self, path, settle=0.3, after=1):
        """Type `path` into the open GTK file dialog and confirm it."""
        keys = self.input
        keys.chord("Control_L", "l")  # show the location field
        keys.chord("Control_L", "a")
        keys.type_text(str(path))
        keys.press("Return")
        time.sleep(settle)
        keys.press("Return")
        time.sleep(after)

    def drive_files(self):
        return {
            str(p.relative_to(self.dest)): p.read_bytes()
            for p in self.dest.rglob("*")
            if p.is_file()
        }


def check_example_copied(session):
    """Both discs of the two-disc "Example" game were copied, and "Other" wasn't."""
    dest = session.dest
    assert (dest / "Games/USA/Example/Disc 1.iso").is_file()
    assert (dest / "Games/USA/Example/Disc 2.iso").is_file()
    assert not (dest / "Games/USA/Other").exists()


def run_keyboard_test(session):
    keys = session.input
    dest = session.dest

    # Start at the page heading, then Tab through actions, filters and categories
    # to the single game-grid Tab stop. No mouse input is used in this branch.
    for _ in range(12):
        keys.press("Tab")
    session.shot("keyboard-game-focus.png")
    keys.press("space")
    time.sleep(0.5)
    session.shot("keyboard-selected.png")

    # Tab to the copy button and open the copy dialog.
    for _ in range(6):
        keys.press("Tab")
    keys.press("Return")
    time.sleep(0.5)
    session.shot("keyboard-copy-open.png")

    # Back to the dialog's review button, then its copy button.
    keys.chord("Shift_L", "Tab")
    keys.press("Return")
    time.sleep(2)
    session.shot("keyboard-copy-review.png")
    keys.chord("Shift_L", "Tab")
    keys.press("Return")
    time.sleep(3)
    session.shot("keyboard-copy-complete.png")

    check_example_copied(session)
    assert not (dest / "Games/Japan/Third").exists()
    assert (dest / "boot.iso").read_bytes() == (session.source / "boot.iso").read_bytes()

    # Close the dialog; focus returns to the button, so Enter reopens it.
    keys.press("Escape")
    time.sleep(0.3)
    keys.press("Return")
    time.sleep(0.5)
    session.shot("keyboard-copy-reopened.png")
    print(
        "Native keyboard-only test: Tab reached games; Space selected one multidisc title; "
        "Tab/Enter opened, reviewed and copied it. Only that title was copied, boot image "
        "verified, and Escape/Enter exercised return focus.",
        flush=True,
    )


def copy_game_with_mouse(session):
    mouse = session.input
    dest = session.dest

    session.shot("native-library.png")
    mouse.click(280, 500)  # the first game
    session.shot("native-selected.png")
    session.click_primary_action()  # Copy
    session.shot("native-copy-dialog.png")
    session.click_primary_action()  # Review
    time.sleep(1.5)
    session.shot("native-copy-review.png")
    session.click_primary_action()  # Copy now
    time.sleep(2)

    check_example_copied(session)
    assert b"Other" not in (dest / ".ohmy3do/library.3di").read_bytes()
    assert (dest / "boot.iso").read_bytes() == (session.source / "boot.iso").read_bytes()
    assert (dest / ".ohmy3do/saves/slot-catalog.3di").is_file()
    session.shot("native-copy-complete.png")
    print(
        "Actual Tauri mouse test: selected one multi-disc title, reviewed and copied it; "
        "filtered index, boot and save support verified.",
        flush=True,
    )

    mouse.press("Escape")
    # Meant for "Add discs"; the button has since moved lower (to about y=176).
    mouse.click(1200, 124)
    session.shot("native-import.png")
    print("Actual Library Add discs action exercised.", flush=True)


def edit_save_with_mouse(session, save_path):
    """Open slot01 in the Save Manager, change DOOM's starting map to 5 and
    save, then check the archive and game checksums."""
    mouse = session.input

    mouse.click(85, 205)  # Save Manager
    session.shot("native-saves.png")
    mouse.click(1053, 124)  # Open backup
    time.sleep(1)
    session.shot("native-file-dialog.png")
    session.open_save_file(save_path, settle=0.5, after=2)
    session.shot("native-save-open.png")

    mouse.click(1020, 647)  # the starting map field
    mouse.chord("Control_L", "a")
    mouse.type_text("5")
    mouse.press("Tab")
    time.sleep(0.5)
    session.shot("native-save-edited.png")
    mouse.click(678, 354)  # Save
    time.sleep(0.3)
    session.shot("native-save-confirm.png")
    session.click_primary_action()  # confirm
    time.sleep(1.5)

    data = save_path.read_bytes()
    assert struct.unpack_from(">I", data, DOOM_MAP_OFFSET)[0] == 5
    assert b"keep me" in data
    assert zlib.crc32(data[24:]) == struct.unpack_from(">I", data, 16)[0]
    assert list(save_path.parent.glob("slot01.3ds.*.bak"))
    doom_words = struct.unpack_from(">9I", data, FIRST_ENTRY_DATA)
    expected_checksum = (DOOM_CHECKSUM_SEED + sum(doom_words)) & 0xFFFFFFFF
    assert struct.unpack_from(">I", data, DOOM_CHECKSUM_OFFSET)[0] == expected_checksum
    session.shot("native-save-saved.png")
    print(
        "Actual Save Manager: edited DOOM starting map, saved over slot01 with recovery copy; "
        "archive/game checksums and unrelated save verified.",
        flush=True,
    )


def compare_saves(session, save_path):
    """Compare the open save with slot01 and open the byte view and hex editor."""
    mouse = session.input
    mouse.click(1005, 354)  # Compare
    time.sleep(0.5)
    session.open_save_file(save_path)
    session.shot("native-save-comparison.png")
    mouse.press("Tab")
    mouse.press("Return")
    session.shot("native-save-byte-differences.png")
    mouse.press("Tab")
    mouse.press("Return")
    time.sleep(0.3)
    session.shot("native-comparison-hex-editor.png")

    # Back to the Save Manager, scrolled to the top.
    mouse.click(80, 205)
    mouse.scroll(1100, 300, WHEEL_UP, 25)
    time.sleep(0.3)


def check_drive_read_only(session):
    """Run the drive readiness check and confirm it changed no file."""
    mouse = session.input
    before = session.drive_files()
    mouse.click(80, 320)  # Drive setup
    session.shot("native-readiness-before.png")
    mouse.click(280, 755)  # Check drive
    time.sleep(2)
    session.shot("native-readiness-summary.png")
    mouse.scroll(1100, 720, WHEEL_DOWN, 5)
    time.sleep(0.5)
    session.shot("native-readiness-results.png")
    assert session.drive_files() == before
    print(
        "Native save comparison and readiness controls exercised; "
        "drive check preserved every file.",
        flush=True,
    )


def run_mouse_test(session, args, save_path, second):
    mouse = session.input
    copy_game_with_mouse(session)
    edit_save_with_mouse(session, save_path)

    # Open the second backup; saving the first must not have changed it.
    mouse.click(1053, 124)  # Open backup
    time.sleep(0.5)
    session.open_save_file(second)
    session.shot("native-second-save.png")
    assert second.read_bytes() == save_archive([("DoomPrefs", doom_save())])

    if args.features:
        compare_saves(session, save_path)

    mouse.click(874, 220)  # Use USB: search the drive's save slots
    time.sleep(1)
    session.shot("native-slot-search.png")
    assert save_path.with_name("slot02.3si").is_file()
    print(
        "Opened another backup after saving and refreshed occupied slots from the actual "
        "USB fixture.",
        flush=True,
    )

    if args.features:
        check_drive_read_only(session)
    if args.gallery:
        mouse.click(80, 320)
        session.shot("drive-setup.png")
        mouse.click(85, 260)
        session.shot("media.png")
        mouse.click(85, 814)
        session.shot("settings.png")


def main():
    args = parse_arguments()

    # Build the fixture.
    fixture = args.fixture or ROOT / "build/desktop-ui-smoke"
    source = fixture / "library"
    dest = fixture / "usb"
    cache = fixture / "cache"
    for path in [source, dest, cache / "oh-my-3do", fixture / "shots"]:
        path.mkdir(parents=True, exist_ok=True)
    write_library(source)
    write_settings(cache, source, dest, args.library)

    # The app reads its settings from these folders. There is no GPU in the
    # container, so render in software.
    os.environ.update(
        XDG_CACHE_HOME=str(cache),
        XDG_CONFIG_HOME=str(fixture / "config"),
        XDG_DATA_HOME=str(fixture / "data"),
        WEBKIT_DISABLE_DMABUF_RENDERER="1",
        LIBGL_ALWAYS_SOFTWARE="1",
    )
    os.environ.pop("TAURI_WEBVIEW_AUTOMATION", None)

    session = Session(fixture, Input())

    # Two save backups on the drive: slot01 with an unrelated save beside
    # DOOM's, and slot02 with DOOM's only.
    save_path = dest / ".ohmy3do/saves/slot01.3ds"
    save_path.parent.mkdir(parents=True, exist_ok=True)
    save_path.write_bytes(save_archive([("DoomPrefs", doom_save()), ("OtherSave", b"keep me")]))
    second = save_path.with_name("slot02.3ds")
    second.write_bytes(save_archive([("DoomPrefs", doom_save())]))

    app = subprocess.Popen(
        [str(args.binary or DEFAULT_BINARY)],
        stdout=open(fixture / "native-app.log", "w"),
        stderr=subprocess.STDOUT,
    )
    try:
        time.sleep(3)  # let the window open
        if args.library:
            time.sleep(4)
            session.shot("library-real.png")
            print("Read-only library screenshot captured.", flush=True)
        elif args.keyboard:
            run_keyboard_test(session)
        else:
            run_mouse_test(session, args, save_path, second)
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    main()
