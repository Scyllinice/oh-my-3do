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

Names come from three places. A built-in database recognizes 118 verified filename
patterns covering 77 game titles, even without a matching game on the USB drive.
The PC tools (`index-media.py`, desktop imports and
**Rebuild search indexes**) inspect ISO files for `/nvram/` filename strings (ignoring case) and
write `.ohmy3do/save-names.txt`. This finds some games' save names; CUE/BIN
images and dynamically constructed names are not covered by this scan. Later
runs reuse a per-disc cache. The menu can learn other names: when
you launch a game it notes the NVRAM contents in `save-snapshot.txt`, and at the
next startup it names any file that game created or changed in
`saves-learned.txt`. Learned names take precedence. A save no one has named
keeps its filename. **A** prepares
a backup of one save; **C** prepares all files, including system settings.

Choose a numbered USB slot. **L/R** changes pages of eight. **P: Find slots**
offers occupied slots, search by game or slot name, a numeric jump
(1–999,999,999), and the full numbered view. Slots need no preallocation or in-memory master catalog;
USB capacity and filesystem/firmware limits constrain how many you can actually
store. The two recovery backups appear on the first page.

**A: Back up one** adds or replaces only the matching game in the destination,
preserving its other entries. An empty slot receives just that save; an invalid
or unreadable destination is refused. **C: All** remains a full-slot replacement.
The menu reads and lists the **existing destination** first; **C** switches
between existing and proposed contents, **A** proceeds to confirmation, and **B**
returns. Single-save merges recheck the reviewed destination before writing.
Every successful write closes/reopens the file and verifies its bytes.

The desktop **Drive setup** creates
`.ohmy3do/saves/` and all other initial support automatically. A drive containing
only `boot.iso` can be completed with **Repair installed menu support**; no game
library copy is needed. Full USB library preparation also creates the folder.
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
are listed on the first page alongside the numbered slots.

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

The shared database supports **DOOM** preferences and campaign unlocking, plus
**Icebreaker** and **Icebreaker 2** completion flags and music-track presets. Unsupported save layouts or invalid game checksums
retain raw editing. The menu loads definitions from `.ohmy3do/save-cheats.3db`; the desktop app can
edit and install them without another ISO update. See the [database guide](save-cheats.md).

## Building, naming and comparing slots

Build a slot one save at a time: **A: Back up one** in Console saves copies the
selected NVRAM file; **P: Copy to slot** inside a USB backup copies its selected
save to another slot. Both merge by filename, preserving unrelated destination
saves. Review and confirm each copy. A USB copy keeps its source unchanged and
returns to that source after saving or cancelling. An occupied slot cannot gain
more than 128 saves or exceed the 64 KiB archive limit.

In the USB slot list, **C: Name** opens the on-screen keyboard. **A** adds a
character, **X** erases, **C** clears, **P** saves, and **B** cancels. Labels allow
24 printable ASCII characters; a blank label restores the filename. Recovery
backups keep their fixed names. Names live separately in
`.ohmy3do/saves/slot-labels.txt` for slots 1–8, and `slot-labels-N.txt` for each
subsequent page (page N starts at slot `8*N+1`). Renaming never edits save data.

**X: Compare** selects the first slot; choose a second and press **A**. The list
shows added, removed, changed and identical saves, paired by filename without
regard to ASCII case. **A** on a save shows changed byte offsets and before/after
values; **Left/Right** pages through differences. `--` means the byte is absent.
Filename case and file type changes are reported even if payload bytes match.
Comparison is read only; **B** returns.

On the desktop, use **New backup**, then **Add saves from…**.
Select entries, review which matching filenames they replace, and add them to
the draft. Repeat with other source backups; **Remove from draft** removes
an unwanted draft entry. **Save as…** writes the assembled slot. To include
NVRAM saves, capture them on the console first. Open an existing slot to extend
it instead. Sources stay unchanged, and a failed merge leaves the draft intact.
The destination still has the same 128-file/64 KiB limit.

The desktop **Compare…** uses the chosen archive as a reference and compares
it with the current draft, including applied edits. It shows known game names,
added/removed/changed/unchanged totals, size and filename/file-type changes,
and the first 512 changed byte offsets per save. Unchanged saves are collapsed.
**Open current save in hex editor** opens that save in the draft; removed saves
are available only in the reference archive. Comparing never writes either file. Numbered slots also have a **Save slot name** control using the
same shared label files, with conflict checks and a recovery copy on replacement.

## Cleaning up desktop recovery copies

After repeatedly replacing backups, open **Save Manager → Manage recovery
copies…** and choose the folder containing them. Review the sizes, dates and
archive validity, select copies, and confirm **Delete selected copies**. Deletion
is permanent. **Select older copies (keep newest)** leaves the newest copy per
save unchecked; if that copy is invalid, the newest valid copy is also kept.
You can change those selections manually.

Only app-created `.3ds.UUID.bak` files are listed. Live `.3ds` slots, console
`recovery.3ds`/`recovery2.3ds`, links and unrelated `.bak` files are excluded.
The app verifies the reviewed copies before deletion and reports partial errors
without deleting the remaining files. A changed copy requires a fresh scan.

## Finding saves across slots

On the desktop, open **Save Manager → Find saves across slots** and select the
USB drive's `.ohmy3do/saves` folder. The app scans all canonical numbered backups
and both recovery archives, builds the console catalog, and shows occupied
slots. Type a game title, raw NVRAM filename, backup filename, or slot label.
Search ignores ASCII case; all space-separated words must match somewhere in
the slot. Multiple games in one archive are all searchable. Opening a result
uses the usual Save/Discard/Cancel protection for unfinished edits.

On the console, open **USB backups → P: Find slots**. Choose **Show occupied
slots**, or **Search games / slot names**, enter text and press **P**. **L/R**
pages through results and **A** opens the selected backup. Choose **Go to slot
number** to reach an unused destination, or **Show all slot numbers** to return
to numbered browsing. The same picker is available when choosing a merge
or backup destination; recovery archives are excluded from destinations.

**Refresh the catalog with the desktop app once for existing backups**, and again
after manually copying, deleting or renaming files. USB preparation also refreshes
it. Console backup writes and slot-label changes maintain their catalog entries;
desktop saves and label changes queue a refresh. The console cannot enumerate
`.3ds` files through the firmware's ISO/CUE browser, so its search covers indexed
backups. It keeps only eight results and one catalog record in memory. Search
time grows with the number of indexed backups; there is no eight-slot catalog cap.

The catalog consists of `saves/slot-catalog.3di` and small `slotNN.3si` /
`recovery.3si` / `recovery2.3si` sidecars. Archives and label files remain
authoritative. CRC checks protect the records and their links; a bad or missing
catalog asks for a desktop refresh rather than showing partial matches. A
verified backup remains saved if catalog maintenance fails. Nonempty damaged
archives appear as invalid in the desktop list; zero-byte firmware probe files
are omitted. A valid archive containing no saves is still an occupied slot.
Desktop catalog rebuilding never writes save archives or NVRAM.

Known names improve identification, not cheat support. Unknown saves keep their
raw filenames and remain searchable. See [save-name sources](save-names.md) for
coverage and how names are selected.

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

In the desktop app, open **Save Manager → Open backup…** and choose
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
numbered `.ohmy3do/saves/slotNN.3ds` file. Back up an occupied slot before replacing
it. Reinstall the drive, cold boot, then select the save in **Tools → Save Manager → USB backups**
and follow the normal restore confirmation and recovery-backup procedure.

[Project overview](../README.md)
