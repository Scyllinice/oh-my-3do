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

Open **CH5 Tools → Edit / update cheat database…**.

1. Choose the library or USB root folder and **Load installed database**, or use
   **Open JSON…** to import a source file. The initial editable copy contains the
   bundled definitions.
2. Choose a game profile. Edit field names, hexadecimal byte offsets, decimal
   ranges, and named presets. Expand **Layout and checksum** for the exact save
   size, signature, and checksum parameters. Add/remove profiles, fields, and
   presets as needed; removing a field also removes presets that depend on it.
3. **Validate** checks structure, bounds, names, overlaps, ranges, and supported
   formats. **Save JSON** keeps an editable source file; **Save JSON as…** switches
   to another source file. Opening another file or closing the editor prompts
   before discarding unsaved changes.
4. **Install database to selected folder** writes both files, verifies readback,
   and records their previous contents in the app's transaction journal. It does
   not replace the boot image or touch saves. Use the app's Undo transaction
   control if you need to restore the previous database.

**Use in Save Manager** makes the current validated definitions available to the
open desktop save editor. Opening a backup automatically loads the nearest
installed database JSON; absent or invalid definitions use the bundled set.
These controls change definitions, not the contents of a save archive.

**Download official definitions** retrieves the project's published JSON on a
worker thread. Offline mode disables downloading. Review the downloaded set,
then **Add missing profiles** to retain your current IDs/values, or explicitly
**Replace matching IDs and add new profiles**. Files are unchanged until you save
or install. A missing/unpublished feed or invalid download reports an error and
keeps local definitions intact. The feed is `data/save-cheats.json` on the
repository's `main` branch, so new official records become downloadable after
publication there. **Add missing bundled definitions** works offline.

Guided setup and USB preparation install the bundled database if none is
present. Existing drive databases are preserved; a JSON source without a console
file can be compiled during preparation. If both staging files exist, they must
agree. Install changes through the database editor to regenerate a mismatched
pair. An existing console-only file is preserved.

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

## Format and adding profiles

The editable schema is demonstrated by
[`data/save-cheats.json`](../data/save-cheats.json). Each record has a unique ID,
game name, exact save filename/size, signature, checksum rule, source URL, notes,
fields, and presets. A preset refers to one field by its zero-based index and
supplies a value within its range. Document original source or reproducible
research for the exact variant before distributing new records.

The initial engine supports aligned 32-bit big-endian fields and a wrapping
32-bit sum of preceding words plus a seed. New records using those rules can be
installed from the desktop without an ISO update. New byte encodings, checksum
algorithms, or multi-field preset operations require extending both engines.

Limits: 128 profiles, 32 fields and 32 presets per profile, 256 KiB JSON, and
64 KiB compiled console data. Labels and identifiers have bounded printable
ASCII lengths; offsets must stay within the save and must not overlap each
other, its signature, or checksum. The binary has a versioned `OM3DCHT1` header,
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
