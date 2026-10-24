# Oh My 3DO Library

Use the desktop app to organize games, prepare a console drive, manage media and
edit save backups. Python and Docker are not needed to run it.

## Install and start

Download the app from the latest **desktop** release on
[Releases](https://github.com/Scyllinice/oh-my-3do/releases).

- **Windows:** run the Setup EXE. It installs for your account without administrator
  access. Setup may need internet access to install Microsoft WebView2.
- **Linux:** run `sh OhMy3DO-Library-Linux-x64-v<version>-Install.run` without sudo.
  GTK 3 and WebKitGTK 4.1 are required; the installer reports missing dependencies.
- **Portable:** extract the complete ZIP into a writable folder and run
  `OhMy3DOLibrary.exe` or `OhMy3DOLibrary`. Keep its support and license files together.

Close the app before reinstalling or uninstalling. On Windows, uninstall through
Installed apps. On Linux, run `sh ~/.local/share/oh-my-3do-library/uninstall.sh`
unless you chose a different installation location. Uninstalling keeps games,
saves and preferences.

## Browse your PC library and console drives

Use **View library** above the game grid to switch between the PC library and
USB/SD drives. **PC folder…** changes the source library; **Browse another drive…**
adds a drive. Each library remembers its own selection and filters.

Use search and filters to find games. Click tiles to select them; Shift-click
selects a range and Ctrl+A selects all shown games. Use arrow keys to move between
game tiles and Space to select; Tab moves to the selection actions. In narrow
windows, those actions are grouped under **Actions…**. **Refresh** rereads a drive
after changes. **Already on drive** indicates which games are present at the
copy destination; it does not verify their contents.

Select titles and choose **Remove…** to review their files before permanent
deletion. This affects only the library being viewed and cannot be undone.
Save backups and copies in other libraries are kept.

## Library and imports

1. Choose a PC library folder.
2. Use **Settings & tools → Set up a PC staging library** to install menu support.
3. In Library, choose **Add discs** and select ISO/CUE files or folders.
4. Choose **Scan discs**, review the matches and destinations, then **Import selected**.

You can also drag source files onto the app. Imports leave the originals intact.
**Fast name-only scan** is quicker but does not verify disc contents. Use
**Identify…** for an incorrect or missing match. To keep a title without a
catalog match, choose its category and check it for import; it is marked
**unverified**. Audio-only CUEs can be imported this way with all referenced tracks.

Select games and use **Move category…** to move them within an organized library.
**Settings & tools → Undo last change** can undo supported library operations.
Permanent title removal is excluded.

## Copy selected games to USB

Select games in the PC library and choose **Copy selected to USB…**. Choose the
drive root, review the file list and required space, then choose **Copy to USB**.
Select all games to copy the entire library.

Copies include all discs and referenced tracks, media, menu and required support.
Unselected games already on the drive remain available. Existing drive settings,
favorites and save backups are preserved. Conflicting disc files block copying
rather than being silently replaced.

**Verify existing disc contents** checks games already on the drive as well as new
copies; it takes longer. Progress and Cancel appear in the active operation.

Use **Safely eject** when finished. The top-bar button targets the displayed
drive, or the PC library's copy destination; hover over it to check the path.
If the reader does not support eject or the drive is busy, close open files and
use the operating system's eject controls. Install the drive with console power off.

## Drive setup without games

**Drive setup** installs everything needed to boot the menu and use Save Manager
without copying any games.

1. Choose the USB/SD drive root.
2. Choose **Use library boot.iso**, **Choose boot.iso…**, or **Download released menu**.
3. Wait for completion, then safely eject.

Use **Repair installed support** to complete a drive that already has `boot.iso`.
Existing settings, backups and media are kept. See [installation](../docs/installation.md)
for manual setup.

**Name / identify drive…** gives a drive a recognizable name and helps prevent
writes to a different card that later uses the same drive letter.

## Checking a drive, copy space and save changes

Use **Drive setup → Check drive** to find missing support files, stale indexes
and media problems. Follow the suggested fixes. The check does not change files
or replace testing on the console.

Copy review shows required and available space. Copying is blocked when there
is not enough room.

**Save Manager → Compare…** shows changes between a reference backup and the
current draft. See [save comparison](../docs/saves.md#building-naming-and-comparing-slots).

## Artwork and previews

Choose a media folder in **Artwork & previews**. **Prepare missing media**
finds missing covers, previews and descriptions; **Rebuild media quality**
recreates prepared media from available originals. You can also assign a cover
or video to a particular game. Video conversion requires FFmpeg and ffprobe;
the app offers to download them.

To update media on an existing drive:

1. Prepare or edit it in the PC library.
2. Switch **View library** to the console drive.
3. Choose **Update media & information…**, review, and apply.

Select games first to update a subset; leave the selection empty to match the
whole drive. This updates media and information without recopying game discs.
Use the normal copy workflow for games not yet on the drive.

See the [media guide](../docs/media-guide.md) for supported sources and full-screen viewing.

## Game information, named drives and slot notes

Select a game and open **Game details** to read its description and other
information. **Get game information…** downloads matching LaunchBox metadata;
**Prepare missing media** also fills missing descriptions. Cached information
works offline.

Use **Game details → Edit information…** to change descriptions, genre,
developer, publisher, release date/year or player count. Downloads preserve
manual edits, including deliberately blank fields. **Restore downloaded** lets
you review and restore the original values.

Select several games and choose **Edit information…** to make shared changes.
Check **Apply** only for the fields you want to change; an applied blank clears
that field. Bulk editing leaves descriptions unchanged.

Useful Library options:

- **Titles & information:** search descriptions, genres, developers and publishers too.
- **All information:** filter for missing information or manual edits.
- **Metadata… → Review information matches…:** choose a different match when the download identifies a game incorrectly.
- **Metadata…:** export or import custom edits, or restore an automatic metadata backup.

Metadata exports and automatic backups contain custom information only, not games
or saves. Copy updated games or use **Update media & information…** to send edits
to the console. There, **hold C and tap P** to read Game details.

For save-slot notes, open a numbered backup in **Save Manager → Notes…**.
Notes are searchable and remain when a slot is reused, so review them when
replacing its contents. Copy the notes file too when moving a backup manually.

## Categories and console visibility

Use **Settings & tools → Manage categories…** to add or rename categories,
choose import destinations, and mark categories **Hide on console**.
**Choose library / drive…** edits another location.

The desktop still shows hidden categories. On the console, **Tools → Settings →
Categories…** shows or hides them without deleting games. Move games out before
removing a populated category.

## Save Manager

Choose **Use USB**, **Use library**, or **Choose folder…** to browse backup slots.
Search by game, filename, slot name or notes. **Refresh** also updates the
catalog used by the console's slot search.

**Open backup…** opens an Oh My 3DO `.3ds` archive; **New backup** starts an empty
draft. Use game-specific controls where available, or the hex editor for raw
changes. Raw edits can make a save unusable; keep an original backup.

**Add saves from…** merges selected saves from another archive. Matching filenames
are replaced; other saves remain. **Remove from draft** removes an entry.

**Save** writes the whole open archive. **Save as…** writes the whole draft to
another file. Saving over a slot replaces that entire archive, not just the
edited game. The app keeps a recovery copy before replacing an existing file.
Editing a USB backup does not change console NVRAM; restore the selected save
on the console afterward.

See the [Save Manager guide](../docs/saves.md) for backup, editing and restore steps.

## Recovery copies, support reports and remembered selections

**Save Manager → Manage recovery copies…** lists app-created recovery copies.
Review and select older copies before deleting them permanently. **Select older
copies (keep newest)** provides a starting selection. Live slots and console
recovery archives are excluded.

**Drive setup → Export support report…** saves drive checks, console diagnostics
and recent app activity. It includes paths but no game or save contents. Review
it before sharing; the app does not upload it.

## Settings and cheat database

**Settings & tools** includes Light/Dark/System appearance, offline mode,
text size (100–200%), startup update preferences, console settings, index rebuilding, library review
and undo. Software graphics is available for display problems and needs an app restart.

**Open database editor…** manages known save fields and cheat presets. Install
changes into a library or drive to use them on the console without rebuilding
the ISO. See the [cheat database reference](../docs/save-cheats.md) for authoring profiles.

## App and menu updates

**Updates** checks desktop and menu releases separately. Startup checks show a
dismissible notice when an update is available. Both startup checks and notices
can be controlled in Settings.

Choose **Update and restart**, or **Update on exit** to keep working. A scheduled
update can be installed with **Restart now** or removed with **Cancel scheduled
update**. Close the app normally to apply an update on exit.

Updates preserve settings and libraries. A failed startup triggers recovery;
**Updates** shows the result and log location. Installed and portable copies
can update without rerunning an installer.

Boot ISO updates are offered for each library or drive that needs one. After
updating one, Updates stays open for the others. Games, settings and saves are
preserved. An unrecognized menu image shows **Version unknown** until identified
by a release check.

For local builds, enable **Settings & tools → Developer updates** and choose the
repository's `iso` folder. Build and package normally, then **Check for updates**.
This works offline and detects desktop rebuilds at the same version. While
enabled, update checks and menu setup use that folder instead of GitHub.
Turn it off to return to published releases.

## Development

See [desktop development](../docs/desktop-development.md) for builds, installers
and tests, and [releasing](../docs/releasing.md) for publication.
