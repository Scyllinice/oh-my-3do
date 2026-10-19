# Save Manager

Open **L+R → Save Manager**. Use **Left/Right** to switch between
**Console saves** and **USB backups**. Console saves lets you inspect internal
saves and create backups; USB backups lets you
inspect a backup slot. Support-file paths below are relative to `.ohmy3do/`
at the USB root.

The **Console saves** view lists files in the console's `/NVRAM` filesystem, their
sizes, and used/free space. Each save shows its game's name when the menu knows
it; the selected save's real filename appears below the list. Viewing and X
refresh are read-only.

Names come from two places. The PC tools (`index-media.py`, desktop imports and
**Rebuild Find index**) inspect ISO files for `/NVRAM/` filename strings and
write `.ohmy3do/save-names.txt`. This finds some games' save names; CUE/BIN
images and dynamically constructed names are not covered by this scan. Later
runs reuse a per-disc cache. The menu can learn other names: when
you launch a game it notes the NVRAM contents in `save-snapshot.txt`, and at the
next startup it names any file that game created or changed in
`saves-learned.txt`. Learned names take precedence. A save no one has named
keeps its filename. **A** prepares
a backup of one save; **C** prepares all files, including system settings.

Choose one of eight USB slots. The menu reads and lists the **existing slot
contents before replacement**. C switches between existing and proposed
contents; A proceeds to a separate confirmation, and B returns. Missing/corrupt
slots are identified as empty or invalid; their contents are not presented as
valid saves. Confirming replaces the chosen slot and verifies the completed
archive by closing/reopening and comparing every byte.

The desktop setup and USB preparation create `.ohmy3do/saves/` automatically.
For a manually prepared drive, create it before using backups, or copy the build's `saves/`
folder into `.ohmy3do/`. The firmware cannot create directories.
Before the first backup or restore in a session, an automatic check writes two
patterns to `saves/usb-test.bin` and verifies both. It does not change NVRAM.
The test file can be deleted on a PC; there is no standalone Tools test option.

The **USB backups** view validates an archive and lists its contents. Select one save
to restore, review its filename/size/type and replacement warning, then confirm.
**Before any NVRAM write**, the menu checks space, captures all current saves,
writes them to `saves/recovery.3ds` and verifies that USB backup. Restoring from
that archive uses `recovery2.3ds` to preserve its source. Both recovery files
are listed alongside `slot01.3ds`–`slot08.3ds`.

Restore allocates space before overwriting, retains the exact length and file
type, reads the file back, and takes a fresh snapshot to check that unrelated
saves are unchanged. It never deletes other saves to make room. The confirmation
warns that the recovery archive is replaced. Keep power on until completion:
replacement is **not atomic**, and interruption can damage the selected save.
Keep a verified USB backup on your PC before restoring saves.

Archives preserve filenames, four-byte types, exact sizes and uncompressed data,
with per-file and whole-payload CRC32 checks. Limits are 128 files and 64 KiB
per archive. They are custom file archives, not raw NVRAM dumps, emulator `.srm`
images, Nintendo 3DS files or Game Guru archives. Invalid names, checksums,
duplicates and lengths reject the whole archive. Files are self-delimiting so an
old USB tail does not require unsupported truncate/seek commands.

## Known values and cheats

Select a save inside a USB backup and press **X: Cheats** for its matching
verified profile. Up/Down selects a preset or known field; A applies a preset,
and Left/Right changes a field. The game checksum updates automatically. C saves
through slot review and confirmation; restoring remains separate. X switches
between known values and the raw hex editor.

The [DOOM weapons code](save-cheats.md#doom-weapons) is entered in-game; weapon
inventory is not stored in its preferences save.

The shared database currently supports **DOOM**: eight preferences fields and
**Unlock campaign levels**. Unsupported save layouts or invalid game checksums
retain raw editing. The menu loads definitions from `.ohmy3do/save-cheats.3db`; the desktop app can
edit and install them without another ISO update. See the [database guide](save-cheats.md).

## Console hex editor

In **Save Manager → USB backups**, open a slot, select a save, and press **C**
for the hex editor. The editor makes an in-memory copy of the complete archive;
opening it and changing bytes do not write to USB or console memory.

- Use the arrows to select a byte and **L/R** to move by 32-byte pages.
- Press **A** to edit a byte. **Left/Right** selects its high or low hexadecimal
  digit; **Up/Down** changes that digit. **A** applies the byte; **B** cancels it.
- Press **C** while browsing bytes to save the edited game. Choose a USB slot,
  review its existing contents, and confirm. Only the matching game save is
  replaced (or added); all other saves already in the destination stay intact.
  An empty slot receives just the edited save. Unreadable slots cannot be
  overwritten from the editor. Choose another slot to preserve the source.
- **B** goes back; unsaved changes require a discard confirmation. **L+R** also
  prompts before leaving unsaved edits.

The editor keeps file names, types, and sizes and rebuilds the archive CRC32
checksums when saving. Raw edits can still invalidate a game's own checksum or
save format; use **X: Cheats** for verified game-specific edits and automatic game checksum repair.
Restore the saved entry separately through the usual restore confirmation.

## Desktop backup browser and editors

In the desktop app, open **CH5 Tools → Save Manager…** and choose
an Oh My 3DO `.3ds` archive. The browser validates the complete archive before
showing its filenames, sizes, four-byte file types and contents. You can export
one raw save, export a single-save `.3ds` archive, or import replacement raw bytes
for the selected save. Filenames and file types remain intact.

Select **Hex editor** when a game-specific editor is shown. The hex editor
shows 256 bytes per page, offsets and an ASCII view. Edit pairs of
hexadecimal digits and choose **Apply hex page** before changing pages or entries.
**Save** also applies a valid pending hex page automatically.
Page edits keep the original byte count; **Import replacement bytes…** can change
a save's size. **Revert selected save** restores that entry; **Discard edits**
restores the last saved archive state. **Open .3ds backup…** stays available while
editing; unfinished edits prompt **Save & open**, **Discard & open**, or **Cancel**
before switching files. Closing the window retains unfinished edits
until the app exits.

**Save** updates the open archive; **Save as…** saves to another chosen filename
and continues editing that file. Both validate the archive, regenerate its CRC32
checksums, publish a complete replacement and verify the written bytes. Replacing
an existing file first keeps its exact original bytes in a uniquely named `.bak`
file beside it; the editor shows the recovery path. Existing export files can
also be replaced with a recovery copy. If the open file changes outside the
editor, saving refuses to overwrite it: reload it or save under another name.
After a successful Save or Save As, the archive is marked saved and another
backup can be opened immediately. Errors and save results appear near the top.

Raw edits may also require an internal game checksum; the archive checksums alone
cannot make an arbitrary edit valid for a game.

The first game-specific editor supports **DOOM 3DO's `DoomPrefs`**: difficulty,
starting map, sound/music volume, control layout, unlocked level, screen size
and detail mode. **Unlock campaign levels** sets the normal campaign limit to 23.
It recognizes the original 40-byte format and verifies its signature, values and
checksum before enabling editing; changes recalculate DOOM's checksum as well.
The layout comes from the [original DOOM 3DO source](https://github.com/Olde-Skuul/doom3do/blob/8a5cdae476e09f2bedd2cc4dcd105dae4b35d4af/source/threedo.c)
(`ReadPrefsFile` / `WritePrefsFile`, big-endian 32-bit words). Other games use
the raw editor until their formats have been verified. This edits the preferences
and level selection DOOM stores, rather than inventing a mid-level save feature.

To restore an edited archive, keep the original on your PC. With the console
off, connect its drive to the PC and put the edited copy into an unused
`.ohmy3do/saves/slot01.3ds`–`slot08.3ds`. Back up an occupied slot before replacing
it. Reinstall the drive, cold boot, then select the save in **Tools → Save Manager → USB backups**
and follow the normal restore confirmation and recovery-backup procedure.

[Project overview](../README.md)
