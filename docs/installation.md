# Installation and support files

Use the matching menu and desktop/media tools from the [latest release](https://github.com/Scyllinice/oh-my-3do/releases/latest).

## Install

Download `boot.iso` from the [latest
release](https://github.com/Scyllinice/oh-my-3do/releases/latest). Back up the
existing `boot.iso` on the USB drive, replace it with the download, and cold
boot the console. Keep games in their existing folders.

The [v1.0.0 release](https://github.com/Scyllinice/oh-my-3do/releases/tag/v1.0.0)
contains the menu, Windows and Linux desktop apps, and portable media tools.

Keep existing settings when upgrading. Favorites, covers, previews, and save
backups are optional.

The release also includes `menu-demo.iso`, an offline interface demo. Copy it to
the USB drive as `boot.iso` to preview the layout without sending ODE commands.
It does not launch games. Stock emulators may run the demo but lack the vendor
commands needed by the normal browser.

## Support files

Keep `boot.iso` at the USB root. Menu-owned files live under `.ohmy3do/`:
`ohmy3do.ini`, favorites, recent history, saved position, diagnostics, search
index, covers, previews, and NVRAM backups. Databases, transaction journals, and
ignored CUE files also live there. Game paths stay relative to the USB volume.
Paths below are relative to `.ohmy3do/` unless shown as full USB paths.

On a fresh drive, create `.ohmy3do/` at the USB root and copy `ohmy3do.ini` from
the media-tools ZIP into it; the desktop app does this when it first imports
into a library. When `.ohmy3do/ohmy3do.ini` cannot be opened, the menu reports
it and uses default settings.

The menu and tools read support files from `.ohmy3do/`. Game files stay in
their category, region and game folders.

The optional `save-cheats.json` source and compact `save-cheats.3db` database
also live in `.ohmy3do/`. Desktop setup and USB preparation install them when
missing; use **Settings & tools → Open database editor…** to edit and install
changes independently of the boot ISO. See the [database guide](save-cheats.md).

## PC-assisted setup

The [desktop app](../desktop/README.md) can prepare a drive **without copying a
library or adding any games**:

1. Open **Drive setup** and select the USB drive root as **USB / SD destination**.
2. Choose **Use library
   boot.iso**, **Choose boot.iso…** for a local download/test build, or
   **Download released menu**. Only the download option needs internet access.
3. Wait for verification. The drive now has `boot.iso`, initial settings,
   `.ohmy3do/saves/`, the cheat database, artwork/preview folders, and initial
   search/catalog support. You can boot the menu and use Save Manager with no
   games installed.

If the drive already has `boot.iso`, **Repair installed support** completes
the layout without downloading an image or copying games. Empty files left by
firmware probes are initialized. The save catalog is refreshed from existing backups. Existing nonempty settings,
Find indexes, save
backups, custom cheat definitions and media are preserved. Installations have
an undo journal. Paths are under `.ohmy3do/`; the backup directory is
`.ohmy3do/saves/`, not a top-level `/saves` folder.

A staging library remains optional: under **Settings & tools**, use **Set up a PC staging library**, choose its folder, and download the matching released menu there.
In **Library**, use **Add discs** to import games. Select the game tiles you
want (or filter by title, category or region and **Select all shown**), then
choose **Copy selected to USB…**. In the dialog, choose the USB destination,
**Review copy**, and **Copy to USB**. The review breaks down games, covers,
previews and support files, distinguishes reviewed bytes from bytes to copy,
and shows free space now and estimated free space after copying. It reserves
working space for filesystem allocation and temporary/support files. An
insufficient-space review disables copying; free space is checked again before
the copy starts. Each game folder
includes all its discs and referenced CUE tracks, with covers/previews copied
automatically. Games already on the USB remain available when unselected; the
menu indexes retain them. Selecting all library tiles copies the full library.
Drive-only setup and repair remain under **Drive setup**. The app needs neither
Python nor Docker at runtime.

## Check a prepared drive

In **Drive setup**, select the destination and choose **Check drive**. This is
read only: it checks the boot image's volume header and size, settings,
categories, game/track paths, search indexes, navigation helpers, save archives
and their catalog, the cheat database, and indexed covers/previews. Each failed
check includes a suggested fix; repair remains a separate action. Missing
optional media is a warning when that media is enabled in console settings.

The report is a snapshot. Run it again after changing the drive. It does not
boot the console, check firmware behavior, or checksum entire game images.
Safely eject the drive after preparing it and install it with console power off.

Use **Export support report…** on the same page to save the app version,
platform, a fresh readiness report, console-exported `.ohmy3do/diagnostics.txt`
(if present), and recent desktop activity in one text file. Paths and diagnostic
text are included, so review the file before sharing. Save/game contents are
excluded. Export does not upload anything or replace the console diagnostics.

## Keeping the app and menu current

The desktop app checks GitHub's latest stable release at startup. **Updates**
can check manually. **Update and restart** downloads the desktop package,
verifies the published SHA-256 checksum, replaces the app after it exits and
relaunches it. Settings and game/save files stay in place. A failed launch restores
the previous executable and program support files. The portable app folder must
be writable.

Boot ISO updates are offered separately, with only relevant actions shown.
Direct installation creates missing support files and preserves existing files,
with an undo journal. Startup checks can be disabled; Offline mode disables
checks and downloads. See [desktop updates](../desktop/README.md#app-and-menu-updates).

## Manual drive layout

A minimal drive can look like this; games can also stay in other folders:

```text
USB/
  boot.iso
  Games/
    Example.iso
  .ohmy3do/
    ohmy3do.ini
    categories.json   # Created by desktop setup; controls category visibility
    saves/
```

Create `.ohmy3do/saves/` before using save backups. See the
[menu guide](menu-guide.md) for controls and settings, the
[media guide](media-guide.md) for covers and previews, and the
[save guide](saves.md) for backup/restore instructions.

[Project overview](../README.md)
