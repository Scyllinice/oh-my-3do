# Set up a console drive

Download from [Releases](https://github.com/Scyllinice/oh-my-3do/releases).
Choose a **menu** release for `boot.iso` and support tools, and a **desktop**
release for the PC app. Their versions are independent.

## Install

The desktop app is the easiest way to prepare a fresh USB/SD drive:

1. Open **Drive setup** and choose the drive root.
2. Choose **Download released menu**, **Use library boot.iso**, or **Choose boot.iso…**.
3. Wait for completion. The app installs the menu and its required support files;
   no games need to be copied.
4. Safely eject, install the drive with the console powered off, and cold boot.

If `boot.iso` is already installed, **Repair installed support** adds missing
support files while keeping settings, media and save backups.

To add games, select them in the desktop Library and choose **Copy selected to
USB…**. Review the destination and space needed, then copy. Games already on the
drive remain available. See the [desktop guide](../desktop/README.md).

## Support files

Keep `boot.iso` at the drive root. Settings, artwork, search indexes and backups
live under `.ohmy3do/`; leave that folder in place. The saves folder is
`.ohmy3do/saves/`, not `/saves`.

Leave `Oh My 3DO - Folder Navigation Helper (Keep).iso` files in place too.
They are hidden in the console menu and help folder navigation work reliably.
Drive setup repairs them if needed.

## PC-assisted setup

A separate PC library is optional. Choose its folder in the desktop app, then
use **Settings & tools → Set up a PC staging library** before importing games.
Use **Add discs** to import, and copy only the games you want on the console.

Use **Update media & information…** while viewing a console drive to refresh its
covers, previews and descriptions without copying game discs again.

## Check a prepared drive

Choose **Drive setup → Check drive** for a report of missing files, stale indexes
and media problems. Follow its suggested fixes. The check does not change files
or replace a console test.

**Export support report…** saves the report, any console diagnostics and recent
app activity. Review it before sharing: it includes file paths, but no game or
save contents.

## Keeping the app and menu current

The desktop app's **Updates** page offers desktop and boot ISO updates separately.
Choose **Update and restart** or **Update on exit** for the app, and update each
library or drive that needs a new boot ISO. Existing games, saves and settings
are preserved.

Startup checks can be disabled in Settings. Offline mode disables GitHub
updates; developer mode can use a local build folder instead. See
[desktop updates](../desktop/README.md#app-and-menu-updates).

To update manually, back up the existing `boot.iso` and replace it with the
new download. Keep the existing `.ohmy3do/` folder and settings.

## Manual drive layout

For a basic manual installation, create `.ohmy3do/` and copy the supplied
`ohmy3do.ini` into it. Create `.ohmy3do/saves/` before using backups.
Games can remain in their existing folders:

```text
USB/
  boot.iso
  Games/
    Example.iso
  .ohmy3do/
    ohmy3do.ini
    saves/
```

Use desktop **Drive setup** for search, media and the complete support layout.
The optional `menu-demo.iso` can be copied as `boot.iso` for an offline interface
demo; it does not launch games.

[Menu guide](menu-guide.md) · [Media guide](media-guide.md) · [Save Manager](saves.md)
