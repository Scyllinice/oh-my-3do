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
missing; use **CH5 Tools → Edit / update cheat database…** to edit and install
changes independently of the boot ISO. See the [database guide](save-cheats.md).

## PC-assisted setup

The [desktop app](../desktop/README.md) can import and organize discs, prepare covers
and previews, edit console settings, and copy a staging library to a USB drive.
On **Prepare USB**, enable **Guided setup for a new library / drive** (also
available from the empty Library view):

1. Choose an existing staging folder on the PC.
2. Choose initial Adult filtering and video-preview preferences, then select
   **Download & install matching menu**. The app downloads `boot.iso` and its
   SHA-256 file from the GitHub release matching the desktop app's version,
   verifies the image, creates settings if missing and builds the Find index.
   Existing settings are preserved; installation has an undo journal.
3. Use **Add discs** to import games, then return to **Prepare USB**, choose a
   separate drive destination, validate, review and copy.

Setup requires internet access. For offline use, install an already downloaded
`boot.iso` through **Tools & settings → Install menu boot.iso…**. The app needs
neither Python nor Docker at runtime. USB preparation creates the saves folder
and preserves existing destination settings and saves.

## Keeping the app and menu current

The desktop app checks for a newer stable release at startup. **Tools → Updates**
can check manually and offers verified downloads of the desktop ZIP and
`boot.iso`. Close the app and extract the ZIP to update the desktop program.
For the console, download the image or install it directly into a checked library
or USB root. Direct installation preserves support files and has an undo journal.
See [desktop updates](../desktop/README.md#app-and-menu-updates) for details.
Startup checks can be disabled; Offline mode disables checks and downloads.

## Manual drive layout

A minimal drive can look like this; games can also stay in other folders:

```text
USB/
  boot.iso
  Games/
    Example.iso
  .ohmy3do/
    ohmy3do.ini
    saves/
```

Create `.ohmy3do/saves/` before using save backups. See the
[menu guide](menu-guide.md) for controls and settings, the
[media guide](media-guide.md) for covers and previews, and the
[save guide](saves.md) for backup/restore instructions.

[Project overview](../README.md)
