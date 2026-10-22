# Save Manager

Use **L+R → Save Manager** on the console. Left/Right switches between
**Console saves** and **USB backups**. The desktop app can browse and edit those
backups too.

Prepare the drive with desktop **Drive setup** first. Backups live in
`.ohmy3do/saves/`. Keep a copy on your PC before editing or restoring saves.

## Back up console saves

In **Console saves**, select a save:

- **A: Back up one** adds or replaces that save in a USB slot, keeping its other saves.
- **C: All** backs up all console saves and replaces the destination slot's contents.

Choose a slot, review the proposed contents and confirm. In the review,
C switches between the existing and proposed contents. Wait for a successful
completion message before removing power.

Use **L/R** to change slot pages or **P: Find slots** to search occupied slots
or jump to a number. Empty slots do not need to be created in advance.

## Restore a save

1. Open **USB backups** and choose a slot.
2. Select the save to restore.
3. Review the replacement warning and confirm.
4. Keep power on until the operation finishes.

Restoring replaces **only the selected save**, not all console NVRAM.
Before writing, the menu backs up the current console saves to `recovery.3ds`.
When restoring from that archive, it uses `recovery2.3ds` instead. Both appear
in the backup list.

If restoration fails, keep the recovery backup and check [troubleshooting](troubleshooting.md).
An interrupted restore can damage the selected save. The menu does not delete
other saves to make room.

## Known values and cheats

Inside a USB backup, select a save and press **X: Cheats** for supported games.
Use Up/Down to choose a preset or field, **A** to apply a preset, and
**Left/Right** to change a field. **C** saves through slot review and confirmation.
Restore that save separately to apply it to the console.

Known editors support DOOM preferences and campaign unlocking, plus Icebreaker
and Icebreaker 2 completion flags and music presets. Unsupported saves use the
raw hex editor. Known editors update supported game checksums automatically.

DOOM's weapon inventory is not stored in its preferences save; use the
[in-game weapons code](save-cheats.md#doom-weapons). See the
[cheat database reference](save-cheats.md) to add or edit profiles.

## Building, naming and comparing slots

Build a slot one save at a time with **A: Back up one** from Console saves, or
**P: Copy to slot** on a save inside a USB backup. Both keep unrelated saves
already in the destination. Each slot holds up to 128 saves and 64 KiB.

In the slot list:

- **C: Name** gives a slot a label. P saves the typed name; B cancels.
- **X: Compare** selects the first slot; choose another and press A to compare.
  A opens a save's differences, Left/Right changes pages, and B returns.

On the desktop, choose **New backup → Add saves from…**, select entries from an
existing backup and repeat with other backups. **Remove from draft** removes
an unwanted entry. Use **Save as…** to save the assembled slot.
To include console NVRAM saves, back them up on the console first.

Desktop **Compare…** compares another archive with the current draft, including
unsaved edits. It does not change either file.

## Finding saves across slots

On the desktop, open Save Manager and choose the drive's `.ohmy3do/saves` folder.
Search by game, save filename, slot name or notes. **Refresh** updates the catalog
used by console search.

On the console, choose **USB backups → P: Find slots** to list occupied slots,
search games or slot names, jump to a slot number, or return to all slot numbers.
Press A to open a result and L/R to change pages.

Refresh with the desktop app after manually copying, deleting or renaming
backups. Backups made on the console update the catalog themselves. A missing or
damaged catalog needs a desktop refresh; the archives remain available.

Known game names help identify saves. Unknown saves keep their original
filenames. See [save-name sources](save-names.md) for coverage.

## Console hex editor

Open **USB backups**, select a save and press **C**.

- Use the arrows to select a byte; **L/R** changes pages.
- **A** edits a byte. Left/Right selects its digit; Up/Down changes the value.
  A applies the byte and B cancels it.
- **C** saves the edited game to a chosen USB slot. Only that game's matching
  save is replaced or added; other destination saves stay intact.
- **B** leaves the editor and asks before discarding unsaved changes.

Choose another slot to preserve the source. Raw changes can invalidate a game's
save format or checksum. Prefer **X: Cheats** when a known editor is available.
Saving edits does not restore them to the console; restore the save separately.

## Desktop backup browser and editors

Choose **Save Manager → Open backup…** to edit an Oh My 3DO `.3ds` archive.
These are not emulator `.srm`, Nintendo 3DS or Game Guru files.

Select a save to use its game-specific controls or **Hex editor**. Apply hex
page edits before changing pages; Save also applies valid pending edits.
**Revert selected save** undoes edits to that entry; **Discard edits** returns
to the last saved archive. Opening another backup prompts about unsaved work.

You can export one raw save or a single-save archive, or import replacement raw
bytes. Keep an original copy: archive verification cannot make arbitrary raw
edits valid for the game.

**Save** writes the complete open archive. **Save as…** writes the complete
draft to another file. Saving over a slot replaces its **entire contents**,
including other games. Use **Add saves from…** to assemble the desired contents
first. Replacing an existing file creates a recovery copy beside it.

To use desktop edits on the console, save to an unused numbered file such as
`.ohmy3do/saves/slot09.3ds`, refresh the slot catalog, safely eject, and reinstall
the drive with console power off. Open that slot on the console and restore
the selected save.

## Cleaning up desktop recovery copies

Open **Save Manager → Manage recovery copies…** and choose their folder.
Review copies, dates and validity, then select and permanently delete unwanted
copies. **Select older copies (keep newest)** helps keep a recent usable copy.

This cleanup excludes live slots and the console's `recovery.3ds` and
`recovery2.3ds` archives. Keep a separate PC backup of saves you care about.

[Project overview](../README.md)
