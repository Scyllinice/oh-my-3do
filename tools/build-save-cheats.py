#!/usr/bin/env python3
"""Validate the shared, source-verified save database and generate its C tables.

Usage: build-save-cheats.py [--check] [--binary FILE] [--source JSON]

data/save-cheats.json describes, for each supported game, how its save file
is laid out: where the values a player might want to change live (fields),
ready-made changes such as "max money" (presets), and how the menu
recognizes the file (a signature) and keeps it valid (a checksum). The same
file is read by the desktop app (desktop/src/save_cheats.rs).

This script checks the database thoroughly and writes it into
src/save_cheat_db.h, the copy built into the menu. --check only confirms
that header is up to date. --binary also writes the compact .3db file the
menu can load from the USB drive instead; with --source, that file is made
from another JSON database, leaving the built-in copy alone.
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "data/save-cheats.json"

MAX_PROFILES = 128
MIN_SAVE_SIZE = 8
MAX_SAVE_SIZE = 32768
MAX_FIELDS = 160
MAX_PRESETS = 32
MAX_ADDITIONAL_CHANGES = 159

# The longest text the menu's tables hold, in characters (each field in the
# binary file has room for one more, a terminating NUL).
MAX_ID_LENGTH = 63
MAX_GAME_LENGTH = 31
MAX_FILENAME_LENGTH = 31
MAX_FIELD_LABEL_LENGTH = 25
MAX_PRESET_LABEL_LENGTH = 34

# The binary file's fixed-size text slots, in bytes.
ID_SLOT = 64
GAME_SLOT = 32
FILENAME_SLOT = 32
FIELD_LABEL_SLOT = 28
PRESET_LABEL_SLOT = 36

# The binary file's record sizes, in bytes.
BINARY_HEADER_SIZE = 24
PROFILE_RECORD_SIZE = 48 + ID_SLOT + GAME_SLOT + FILENAME_SLOT  # 176
FIELD_RECORD_SIZE = FIELD_LABEL_SLOT + 5 * 4  # 48
PRESET_RECORD_SIZE = PRESET_LABEL_SLOT + 3 * 4  # 48
CHANGE_RECORD_SIZE = 2 * 4  # 8
MAX_BINARY_SIZE = 65536

ALL_BITS = 0xFFFFFFFF


def check_text(text, limit):
    """Text must be present, short enough, and printable ASCII."""
    assert text and len(text) <= limit and all(32 <= ord(c) < 127 for c in text)


def check_profile_texts(profile):
    check_text(profile["id"], MAX_ID_LENGTH)
    check_text(profile["game"], MAX_GAME_LENGTH)
    check_text(profile["filename"], MAX_FILENAME_LENGTH)
    for field in profile["fields"]:
        check_text(field["label"], MAX_FIELD_LABEL_LENGTH)
    for preset in profile["presets"]:
        check_text(preset["label"], MAX_PRESET_LABEL_LENGTH)

    # The file name is looked up inside the game's save folder, so it must
    # be a plain name.
    filename = profile["filename"]
    assert filename not in (".", "..") and not any(c in filename for c in "/\\:")


def check_checksum(profile):
    """A "sum32be" checksum is the 32-bit word at the end of the file. With
    "none" the offset marks the end of the file and the seed is zero."""
    checksum = profile["checksum"]
    algorithm = checksum["algorithm"]
    assert algorithm in ("sum32be", "none")

    checksum_size = 4 if algorithm == "sum32be" else 0
    assert checksum["offset"] == profile["size"] - checksum_size

    assert 0 <= checksum["seed"] <= ALL_BITS
    assert algorithm != "none" or checksum["seed"] == 0


def check_signature(profile):
    """The signature is a 1, 2 or 4-byte value that identifies the file, and
    must sit before the checksum."""
    signature = profile["signature"]
    width = signature["width"]
    end_of_data = profile["checksum"]["offset"]
    assert width in (1, 2, 4)
    assert 0 <= signature["offset"] <= end_of_data - width
    assert 0 <= signature["value"] < 1 << (8 * width)


def check_fields(profile):
    """Check each field's position, mask and range, and that no two fields
    (or a field and the signature) claim the same bits."""
    signature = profile["signature"]
    end_of_data = profile["checksum"]["offset"]

    # One byte per save byte, recording which of its bits are already used.
    occupied = bytearray(profile["size"])
    signature_end = signature["offset"] + signature["width"]
    occupied[signature["offset"] : signature_end] = bytes([255]) * signature["width"]

    for field in profile["fields"]:
        field.setdefault("width", 4)
        field.setdefault("mask", ALL_BITS)
        width = field["width"]
        mask = field["mask"]
        assert width in (1, 2, 4)
        assert 0 <= field["offset"] <= end_of_data - width
        assert 0 < mask < 1 << (width * 8)

        # The mask must be one run of set bits. Shifting it down to bit 0
        # (dividing by its lowest set bit) then gives all ones, which is also
        # the largest value the field can hold.
        lowest_bit = mask & -mask
        largest_value = mask // lowest_bit
        assert largest_value & (largest_value + 1) == 0
        assert 0 <= field["min"] <= field["max"] <= largest_value

        for i, bits in enumerate(mask.to_bytes(width, "big")):
            position = field["offset"] + i
            assert not occupied[position] & bits
            occupied[position] |= bits


def check_presets(profile):
    """Each preset sets its main field and optionally more ("additional"),
    each field at most once and only to a value within its range."""
    fields = profile["fields"]
    for preset in profile["presets"]:
        preset.setdefault("additional", [])
        assert len(preset["additional"]) <= MAX_ADDITIONAL_CHANGES

        # The preset itself is its first change.
        used = set()
        for change in [preset, *preset["additional"]]:
            assert 0 <= change["field"] < len(fields) and change["field"] not in used
            used.add(change["field"])
            field = fields[change["field"]]
            assert field["min"] <= change["value"] <= field["max"]


def check_profile(profile):
    assert profile["encoding"] in ("u32be", "be")
    assert MIN_SAVE_SIZE <= profile["size"] <= MAX_SAVE_SIZE and profile["size"] % 4 == 0
    profile["signature"].setdefault("width", 4)

    check_checksum(profile)
    check_signature(profile)
    assert profile["source"].startswith("https://") and profile["notes"]
    check_profile_texts(profile)
    assert 1 <= len(profile["fields"]) <= MAX_FIELDS
    assert 1 <= len(profile["presets"]) <= MAX_PRESETS
    check_fields(profile)
    check_presets(profile)


def normalized(db):
    """Return a checked copy of the database with optional values filled in
    (widths of 4, full masks, empty "additional" lists).

    Raises AssertionError at the first problem. Ids must be unique, and so
    must each (file name, size, signature) layout, so the menu can't match
    one save file to two profiles.
    """
    db = json.loads(json.dumps(db))  # a deep copy, so the caller's stays as it was
    assert db["version"] == 1 and 1 <= len(db["profiles"]) <= MAX_PROFILES

    ids = set()
    layouts = set()
    for profile in db["profiles"]:
        assert profile["id"] not in ids
        ids.add(profile["id"])

        check_profile(profile)

        signature = profile["signature"]
        layout = (profile["filename"], profile["size"], signature["offset"], signature["value"])
        assert layout not in layouts
        layouts.add(layout)
    return db


def has_checksum(profile):
    """1 when the menu must update a checksum after editing, otherwise 0."""
    return int(profile["checksum"]["algorithm"] != "none")


def c_field_table(index, profile):
    """C lines for one profile's SaveCheatField array:
    {label, offset, min, max, width, mask}."""
    lines = [f"static const SaveCheatField cheat_fields_{index}[] = {{"]
    for field in profile["fields"]:
        lines.append(
            "    {%s,%d,%dUL,%dUL,%d,%dUL},"
            % (
                json.dumps(field["label"]),
                field["offset"],
                field["min"],
                field["max"],
                field["width"],
                field["mask"],
            )
        )
    lines.append("};")
    return lines


def c_preset_tables(index, profile):
    """C lines for a profile's presets: a SaveCheatChange array for each
    preset with additional changes, then the SaveCheatPreset array
    {label, field, value, additional changes, count}."""
    lines = []
    for preset_index, preset in enumerate(profile["presets"]):
        if preset["additional"]:
            lines.append(
                f"static const SaveCheatChange cheat_changes_{index}_{preset_index}[] = {{"
            )
            for change in preset["additional"]:
                lines.append("    {%d,%dUL}," % (change["field"], change["value"]))
            lines.append("};")

    lines.append(f"static const SaveCheatPreset cheat_presets_{index}[] = {{")
    for preset_index, preset in enumerate(profile["presets"]):
        if preset["additional"]:
            changes = f"cheat_changes_{index}_{preset_index}"
        else:
            changes = "NULL"
        lines.append(
            "    {%s,%d,%dUL,%s,%d},"
            % (
                json.dumps(preset["label"]),
                preset["field"],
                preset["value"],
                changes,
                len(preset["additional"]),
            )
        )
    lines.append("};")
    return lines


def c_profile_row(index, profile):
    """One SaveCheatProfile entry, in the struct's field order (save_cheats.h)."""
    signature = profile["signature"]
    checksum = profile["checksum"]
    return "    {%s,%s,%s,%d,%d,%dUL,%d,%dUL,cheat_fields_%d,%d,cheat_presets_%d,%d,%d,%d}," % (
        json.dumps(profile["id"]),
        json.dumps(profile["game"]),
        json.dumps(profile["filename"]),
        profile["size"],
        signature["offset"],
        signature["value"],
        checksum["offset"],
        checksum["seed"],
        index,
        len(profile["fields"]),
        index,
        len(profile["presets"]),
        signature["width"],
        has_checksum(profile),
    )


def generate(db):
    """Return the text of src/save_cheat_db.h for a database.

    Labels and names are written with json.dumps, which gives valid C string
    literals since they are plain printable ASCII.
    """
    db = normalized(db)
    lines = ["/* Generated by tools/build-save-cheats.py; edit data/save-cheats.json. */"]
    for index, profile in enumerate(db["profiles"]):
        lines.extend(c_field_table(index, profile))
        lines.extend(c_preset_tables(index, profile))

    lines.append("static const SaveCheatProfile cheat_profiles[] = {")
    for index, profile in enumerate(db["profiles"]):
        lines.append(c_profile_row(index, profile))
    lines.append("};")
    return "\n".join(lines) + "\n"


def text_slot(value, size):
    """ASCII text padded with NULs to a fixed-size slot."""
    return value.encode("ascii").ljust(size, b"\0")


def binary_profile(profile):
    """One profile's record in the .3db file (all numbers big-endian 32-bit).

    The record starts with its own length, so the menu can skip it, then the
    save's layout, the counts, and the id, game and file name. Its fields
    and presets follow, each preset followed by its additional changes.
    """
    signature = profile["signature"]
    checksum = profile["checksum"]

    presets_size = 0
    for preset in profile["presets"]:
        presets_size += PRESET_RECORD_SIZE + CHANGE_RECORD_SIZE * len(preset["additional"])
    length = PROFILE_RECORD_SIZE + FIELD_RECORD_SIZE * len(profile["fields"]) + presets_size

    record = bytearray()
    record += struct.pack(
        ">12I",
        length,
        profile["size"],
        signature["offset"],
        signature["value"],
        checksum["offset"],
        checksum["seed"],
        len(profile["fields"]),
        len(profile["presets"]),
        signature["width"],
        has_checksum(profile),
        0,  # reserved
        0,  # reserved
    )
    record += text_slot(profile["id"], ID_SLOT)
    record += text_slot(profile["game"], GAME_SLOT)
    record += text_slot(profile["filename"], FILENAME_SLOT)

    for field in profile["fields"]:
        record += text_slot(field["label"], FIELD_LABEL_SLOT)
        record += struct.pack(
            ">5I", field["offset"], field["min"], field["max"], field["width"], field["mask"]
        )

    for preset in profile["presets"]:
        record += text_slot(preset["label"], PRESET_LABEL_SLOT)
        record += struct.pack(">3I", preset["field"], preset["value"], len(preset["additional"]))
        for change in preset["additional"]:
            record += struct.pack(">2I", change["field"], change["value"])

    return record


def binary(db):
    """Return the .3db file for a database.

    Header: "OM3DCHT2", then the file size, profile count, CRC-32 of the
    profile records and a reserved zero (big-endian, 32 bits each).
    """
    db = normalized(db)
    payload = bytearray()
    for profile in db["profiles"]:
        payload += binary_profile(profile)

    total_size = BINARY_HEADER_SIZE + len(payload)
    assert total_size <= MAX_BINARY_SIZE

    header = struct.pack(">4I", total_size, len(db["profiles"]), zlib.crc32(payload), 0)
    return b"OM3DCHT2" + header + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--binary", type=Path, help="Export the compact console database")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    args = parser.parse_args()

    is_custom_source = args.source != DEFAULT_SOURCE
    if is_custom_source and not args.binary:
        parser.error(
            "--source requires --binary; custom exports do not replace the bundled fallback"
        )

    # Generating the header also validates the database.
    db = json.loads(args.source.read_text())
    output = generate(db)

    if args.binary:
        args.binary.parent.mkdir(parents=True, exist_ok=True)
        args.binary.write_bytes(binary(db))
    if is_custom_source:
        print("Custom console database exported; bundled fallback unchanged")
        return

    target = ROOT / "src/save_cheat_db.h"
    if args.check:
        assert target.read_text() == output, "Run python3 tools/build-save-cheats.py"
    else:
        target.write_text(output)
    status = "current" if args.check else "generated"
    print("Save cheats: shared database validated; C tables " + status)


if __name__ == "__main__":
    main()
