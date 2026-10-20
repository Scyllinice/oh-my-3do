# Save cheat database

The console loads **`.ohmy3do/save-cheats.3db`** from the USB drive when Save
Manager first reads the save information. The desktop app edits the companion
**`.ohmy3do/save-cheats.json`** and generates the compact console file. Updating
these definitions does **not** require rebuilding or replacing `boot.iso`.
Start the console again after changing the drive's database.

The cheats view identifies its active source as **USB database** or **bundled**.
The menu retains bundled definitions as a fallback when the external database
is missing or invalid. A valid external database replaces the bundled set for
that session, including any profiles you changed or removed. The console reads
only the compact file; copying an edited JSON alone will not update it.

## Desktop editing and updates

Open **Settings & tools → Open database editor…** in the desktop app.

1. The editor starts with bundled definitions. Use **Open JSON…** to load the
   installed `.ohmy3do/save-cheats.json` or another custom database.
2. Select a game profile and edit names, addresses, widths, masks, ranges and
   presets. Add/remove profiles, fields and presets as needed. Advanced JSON
   exposes signatures, checksums and multi-field presets. **Validate** checks the
   draft, and saving/installing always validates it too;
   invalid bounds, overlaps and unsupported formats are rejected.
3. **Save JSON…** writes a validated source file. **Install to drive…** writes
   both JSON and compact console files into a selected library/drive and verifies
   readback. Installation is recorded in the undo journal; it does not replace
   the boot image or touch saves.
4. **Merge published profiles** downloads the official JSON from
   `data/save-cheats.json` on the project's `main` branch and adds missing IDs,
   preserving your existing profiles. Save or install to publish the result.
   Failed downloads leave the local database intact.

Opening another JSON or closing the editor prompts before discarding unsaved
edits. Opening a backup loads the nearest installed JSON; absent or invalid
sources use bundled definitions. Reopen the backup after installing changed
definitions, or **Use in Save Manager** activates validated definitions for the
current desktop session without writing files. These controls edit definitions,
not save archive contents.

Drive setup and USB copying install bundled definitions when absent. Existing
custom databases are preserved. A JSON source without its console file can be
compiled during preparation; when both staging files exist they must agree.
Use the editor to regenerate a mismatched pair. Existing console-only files
are preserved.

## Supported saves and controls

Currently supported: **DOOM**, `DoomPrefs`, the source-verified 40-byte big-endian
preferences format. Eight fields cover difficulty, starting map, sound/music
volumes, controls, unlocked level, screen size, and detail. **Unlock campaign
levels** writes 23 to offset `0x0018`; the internal game checksum at `0x0024`
is repaired automatically. This file does not save health, ammunition, or
mid-level state. The [original game source](https://github.com/Olde-Skuul/doom3do/blob/8a5cdae476e09f2bedd2cc4dcd105dae4b35d4af/source/threedo.c#L395-L486)
defines the format; `source/data.c` and `source/game.c` in that revision establish
the campaign unlock value.

### DOOM weapons

Weapon inventory is runtime state, absent from `DoomPrefs`; selecting a later
starting map still starts with the pistol. The database cannot grant weapons by
editing that preferences file.

Use DOOM's built-in code instead: open the automap with **Use + P** (**B + P**
with the default control layout), release those buttons, then enter:

**A, Left, A, B, A, Right, A, C, A**

Left and Right are **D-pad directions**, not shoulder buttons. This grants all
weapons, 500 ammo of each type, keycards, and 200 armor. The original source's
[`AM_Control` and cheat table](https://github.com/Olde-Skuul/doom3do/blob/8a5cdae476e09f2bedd2cc4dcd105dae4b35d4af/source/ammain.c#L249-L307)
implement this in-game action; it is not a persistent save unlock.

On the console, open a USB backup and press **X: Cheats** on the selected save.
Use Up/Down to choose a preset or field. **A** applies a selected preset; it does not change field values. **Left/Right**
changes a selected field within its range. The footer shows the relevant control. **X** switches to raw hex editing, **C** saves
through slot review and confirmation. Saving adds or replaces only the edited
game in the destination slot, preserving its other saves; an empty slot receives
just that game. **B** goes back with a discard prompt for unsaved changes. Restore a saved entry separately to change console memory.

On the desktop, **Game editor** shows known fields, offsets, values, presets,
and format provenance. Save the archive using the usual Save or Save as controls.
These addresses are offsets inside individual save files, not running-game RAM
addresses, offsets in the `.3ds` archive, or imported Game Guru codes.

Profiles match the filename, exact length, signature, field ranges, and internal
checksum. Structural validation cannot establish that a researched offset or
value has the intended game effect. Unsupported layouts retain raw hex editing;
raw edits do not automatically repair game checksums.

## Icebreaker and Icebreaker 2

Both bundled profiles support the 84-byte level chart: a 16-bit developer ID,
150 completion nibbles, and masked music-track bits. Each level value combines
completion flags: 1 easy, 2 medium, 4 hard, 8 insane; 15 means all difficulties.
Presets mark the first 30 or all 150 levels complete, or enable every music track.
The edits preserve the neighboring level, unused chart bytes, padding, and current
difficulty bits. These game formats have no internal checksum; archive CRCs are
still regenerated when saving.

Offsets and presets follow the original released source, pinned in each profile's
source link in [the database](../data/save-cheats.json). These definitions are
source-verified and tested in both editing engines; they have not been tested
on physical hardware. Use matching updated menu and desktop builds for these
profiles. An installed USB database is authoritative: load the new bundled JSON
in the desktop database editor and install it if your USB still has older definitions.

## Format and adding profiles

The editable schema is demonstrated by
[`data/save-cheats.json`](../data/save-cheats.json). Each record has a unique ID,
game name, exact save filename/size, signature, checksum rule, source URL, notes,
fields, and presets. A preset refers to a field by its zero-based index and supplies a value within
its range. Its optional `additional` array applies more `{field, value}` changes
as one validated preset. Document original source or reproducible
research for the exact variant before distributing new records.

The engine supports 1-, 2-, and 4-byte big-endian fields, contiguous bit masks,
and either a wrapping 32-bit sum of preceding words plus a seed or an explicitly
checksum-free layout. `width` defaults to 4 and `mask` to `4294967295`; a masked
value is shifted down to bit zero in the editor. Disjoint masks can share a byte,
while overlapping bits and signature/checksum writes are refused. Signatures
also accept a 1-, 2-, or 4-byte width. Checksum-free profiles use the save size as
the checksum offset and a zero seed. New encodings/checksum algorithms still
require extending both engines.

Limits: 128 profiles, 160 fields and 32 presets per profile, 256 KiB JSON, and
64 KiB compiled console data. Labels and identifiers have bounded printable
ASCII lengths; offsets must stay within the save and must not overlap each
other, its signature, or checksum. The binary has a versioned `OM3DCHT2` header (the updated console also reads `OM3DCHT1`),
logical length, and payload CRC32. The loader validates the complete candidate
before activating it and ignores any physical USB tail beyond its logical size.
Only bounded data records are loaded; definitions contain no executable code.

For development, `python3 tools/build-save-cheats.py --binary PATH` exports a
console file from the bundled JSON. `--source FILE` accepts another JSON source;
use the desktop editor to validate and install custom records. The generated C
tables in `src/save_cheat_db.h` are the boot image's fallback, regenerated with
`python3 tools/build-save-cheats.py`. Run `make test` and desktop tests after
engine changes. Tests cover parser rejection, source preservation, rollback,
checksums, and desktop-to-console file interoperability.
