# Oh My 3DO Library

A Tauri 2 desktop app with an HTML/CSS interface and the shared Rust library
engine. Game imports, media conversion, USB transfers, save editing and
verification run in Rust. No Python or Node installation is needed to use it.

## Install and start

**Windows:** run `OhMy3DO-Library-Windows-x64-v<version>-Setup.exe`. It installs for
your account under `%LOCALAPPDATA%\Programs\OhMy3DOLibrary`, adds Start menu
shortcuts and an entry in Windows Installed apps, and offers an optional desktop
shortcut. No administrator account is required. Setup checks Microsoft WebView2
and runs Microsoft's Evergreen bootstrapper if it is missing; that step needs
an internet connection. Close the app before reinstalling or uninstalling. Use
Windows Installed apps or the Start menu's Uninstall shortcut to remove it.

**Linux:** run `sh OhMy3DO-Library-Linux-x64-v<version>-Install.run` as your normal
user, without sudo. It checks the package checksum and missing dynamic libraries,
then installs in `$XDG_DATA_HOME/oh-my-3do-library` (normally
`~/.local/share/oh-my-3do-library`) and adds an application-menu entry. GTK 3 and
WebKitGTK 4.1 must already be installed. The installer does not install system
packages or require Python. Run `sh ~/.local/share/oh-my-3do-library/uninstall.sh`
to uninstall; adjust the path if you use a custom XDG data directory. Install and
uninstall scripts accept `--yes` for unattended use. `--help` describes the installer.

Both installers use a writable per-user application folder, so the existing
verified ZIP-based self-updater continues to work. Uninstall removes application
files and shortcuts, while keeping games, saves, preferences, metadata snapshots
and the shared WebView2 Runtime. Extra files placed in the app folder are kept,
except the application's dependency-notice directory and owned update remnants.

**Portable:**
Extract the complete Windows or Linux ZIP into a writable folder and start
`OhMy3DOLibrary.exe` (Windows) or `OhMy3DOLibrary` (Linux). Keep the license
files beside it. Windows requires Microsoft Edge WebView2, normally present on
Windows 10/11. Linux requires GTK 3 and WebKitGTK 4.1, for example Debian's
`libgtk-3-0` and `libwebkit2gtk-4.1-0` packages. The Linux ZIP does not bundle
those system libraries.

Preferences, catalogs, undo journals and update downloads live in the platform
cache directory under `oh-my-3do`. The app continues to use
`native-settings.json`. Changing the GUI does not change disc or save formats.

## Browse your PC library and console drives

Use **View library** above the game grid to switch between your PC library and
USB/SD drives. The copy destination appears in the selector; **Browse another
drive…** remembers additional locations. **PC folder…** changes the source
library used for imports, media and metadata editing. Each view keeps its own
filters and selection.

Browsing does not change drive files. It scans actual ISO/CUE files and reads available
console artwork and information indexes, without requiring a desktop database
on the card. The grid shows disc counts, game sizes and missing covers/previews;
the summary shows free space. Boot images and navigation helpers are excluded.
Use **Refresh** after connecting, replacing or changing a drive.

To update an existing drive, choose **Update media & information…** in its
library view. Select games first to update a subset, or leave the selection
empty to match the entire drive. It transfers prepared PC media, descriptions
and metadata, plus console support, after a file/space review. Optionally find
missing media and descriptions first. Unmatched paths are listed and skipped;
missing disc files require the normal copy workflow. Game disc files are never
copied or replaced by this update. The copy destination setting stays unchanged.
To revise text or assign new artwork, edit the game in the PC library, then run
this update again.

Select one or more titles and choose **Remove…** to review their disc, track and
associated media files. **Permanently remove** deletes them from the library
being viewed and rebuilds its menu indexes. Shared artwork, unrelated files,
save backups and copies in other libraries are kept. Existing save-name mappings
are retained without scanning the remaining discs. Removal cannot be undone;
review the library path and file list before confirming. Cancellation before
final deletion rolls back staged files and index changes.

In the PC view, **Already on drive** means every disc and referenced CUE track
exists at the same relative path with the same file size on the copy destination.
Partial multidisc copies are marked separately. This is a presence check, not a
checksum verification. Browsing another drive does not change the copy
destination; choose that separately in the copy dialog or Drive setup.

Windows eject targets the selected card's removable media, leaving other reader
slots connected. It locks the volume before dismounting and ejecting it; if
Windows denies direct access, it asks Explorer to eject that volume. Success
requires confirming that the volume is unavailable. Cards containing multiple
mounted volumes and drives that do not expose removable media require Windows'
eject controls. Readers may not support individual media eject.

Long operations show live status, a progress bar and cancellation controls in the
active dialog. Copying and verification use byte-based progress; review/library
scans and media batches use file/game counts. Downloads use the server's size
when available. Phases with unknown totals show an indeterminate bar. Errors
and cancellation results stay readable in the dialog, including in Activity.

## Checking a drive, copy space and save changes

**Drive setup → Check drive** produces a read-only readiness report with
suggested fixes for missing or invalid support, stale indexes/catalogs, and
missing or damaged indexed media. It checks navigation helpers as well. A
passing report validates files and layout; it does not prove console booting or
checksum the full game images. See [drive checks](../docs/installation.md#check-a-prepared-drive).

**Library → Copy selected to USB… → Review copy** shows games/covers/previews/
support sizes, bytes to transfer, current free space, a working-space reserve
and estimated free space afterward. Copying is blocked if the estimate does not
fit, and free space is checked again before writes begin.

After a verified copy, choose **Safely eject drive** in the completion dialog.
It is also available in the top bar throughout the app and under **Drive setup**.
The top bar targets the drive being viewed, or the PC library’s copy destination;
hover over it to see the path. Windows requests safe device removal
and confirms the volume is unavailable. Linux needs `udisksctl` (udisks2) and
refuses drives with other mounted volumes. Busy, unsupported, fixed Windows
drives and WSL mounts may require the operating system’s eject controls. The
app reports failures; it never forces an unmount. Keep the app and its cache
off the drive being removed.

**Library → Prepare missing media** works on the selected titles, including
all their discs. It finds missing covers, previews and descriptions, creates or
repairs the console description files, retains existing descriptions/media, and
leaves unselected games alone. The configured media folder and download
preferences still apply. Descriptions use the cached LaunchBox download offline;
if metadata is unavailable, artwork preparation continues and logs the reason.

The Library’s media filter shows **Missing covers**, **Missing previews**, or
**Missing covers or previews**. It checks whether the associated files exist;
a multi-disc title is included if any disc is missing that media. Use the drive
readiness check for damaged files.

**Save Manager → Compare…** compares a chosen reference backup to the current
draft. Known game names and change totals make affected saves easier to find;
expand a row for byte differences or open its current save in the hex editor.
See [save comparison](../docs/saves.md#building-naming-and-comparing-slots).

## Recovery copies, support reports and remembered selections

**Save Manager → Manage recovery copies…** reviews app-created
`save.3ds.UUID.bak` files in a folder you choose. It shows size, date and archive
validity. Nothing is selected initially. **Select older copies (keep newest)**
keeps the newest copy per save and also the newest valid copy if a newer copy
is malformed. You can adjust the selection before confirming permanent deletion.
Current slots, `recovery.3ds`, `recovery2.3ds` and unrelated backup files are not
cleanup candidates. Copies changed since the scan must be rescanned.

**Drive setup → Export support report…** saves a plain-text report with the
app version/platform, a fresh readiness check, existing console diagnostics and
recent desktop activity. It includes paths and diagnostic text, but excludes
save/game payloads. Review it before sharing. The app does not upload it.
Existing reports can be replaced; unrelated documents and the console's
original diagnostics cannot be overwritten by report export.

Library selections, search text, category, region and missing-media filters are remembered
per library on this PC. Switching libraries restores each library's own view;
removed games and unavailable categories/regions are dropped from the saved
view. **Clear selection** is remembered too.

## Game information, named drives and slot notes

Select one Library tile and choose **Game details** to read its description,
genre, developer, publisher, release date and disc paths. **Get game information…**
loads matching LaunchBox metadata; an existing download works offline. Ambiguous
or unmatched titles keep their previous information. **Prepare missing media**
also fills missing descriptions, so a separate information download is optional.
New imports also retain
available descriptions. Copy selected games after refreshing to transfer optional
`.ohmy3do/info/*.txt` files. Console descriptions are plain text, limited to 2048
ASCII bytes; the desktop retains the full original text and source attribution.
Choose **Game details → Edit information…** to edit descriptions, genre,
developer, publisher, release date, release year and player count. Only changed
fields are applied to all discs of that game. Edits, including deliberately cleared
fields, are retained by later metadata downloads. Source attribution identifies
edited descriptions. Use **Restore downloaded** beside a field or
**Restore downloaded information…** for the whole game. Review the current and
downloaded values, choose **Use downloaded values**, then **Save information**.
Restoring removes manual overrides for the restored fields, allowing subsequent
downloads to update them again. Fields not supplied by LaunchBox are kept;
matching multi-disc games restore each disc's own downloaded values. Cached
metadata works offline. Canceling the preview leaves the draft unchanged.
Saving checks for concurrent metadata changes, regenerates
console metadata and description files, and supports the normal Undo command.
The desktop accepts descriptions up to 64 KB; the console copy uses 2048 ASCII bytes.

The Library's **All information** filter can show games missing descriptions,
release dates/years, genres, developers, publishers or player counts, or games
with manual edits. **Missing any information** matches any of those missing
fields on any disc. A release date or year satisfies the release filter.
Deliberately cleared fields still count as missing. This filter combines with
search, region, category and missing-media filters and is remembered per library.

**Edited** markers in the Library, game details and information editor identify
manual values protected from downloads. The editor distinguishes fields edited
on only some discs, newly staged edits and downloaded values staged for restore.
Select games and choose **Edit information…** in the selection bar to set shared
genre, developer, publisher, release date/year and player count values. Check
**Apply** for each field you want to change. Unchecked fields and descriptions
are kept; an applied blank clears the field. Mixed values are identified and the
changes are confirmed before saving. The whole bulk change supports Undo.

Choose **Metadata…** in the Library to export all or selected games' custom
metadata to a portable JSON file, or to import one. Exports contain manual edits
only, including intentional blanks; they contain neither downloaded metadata nor
game/save contents. Keep the export outside the internal `.ohmy3do` folder.
Imports work offline and show current and imported values before applying checked
entries. Conflicting manual edits start unchecked. Matches prefer relative disc
paths and verify title, region and disc; moved entries require a unique match on
those same identifiers. Ambiguous, unmatched and already matching entries are
listed as skipped. Unsupported versions or invalid fields reject the import.
Only supplied fields change; they become protected manual edits. Changes use one
undoable operation and regenerate the console metadata and description files.
Copy the affected games afterward to update the console drive. Export/import
files are limited to 16 MB; use selected exports for larger custom collections.

The Library search has **Titles only** and **Titles & information** scopes.
The broader scope also searches descriptions, genres, developers and publishers
from every disc in a grouped game. Search scope is remembered per library and
combines with the existing filters.
**Clear filters** resets search, category, region and information/media filters
without discarding selections or changing search scope. The heading shows how
many games match; the selection bar identifies selected games outside the current
view so you can see what a copy or bulk edit will include.

Library controls wrap into consistent rows on smaller windows. Dialog contents
scroll while their action buttons stay visible, and space is reserved below the
game tiles when the selection bar wraps. Long-running operations show start,
progress and completion messages in the status bar and Activity. Hover over the
status message to read its full text. Cancel changes to **Canceling…** while the
current step finishes; cancellation is reported separately from a failure.

**Metadata… → Review information matches…** lists games with no automatic match,
ambiguous titles, or previously chosen LaunchBox entries missing from the cached
data. Enable **Show matched games too** to change an existing choice. Choose a
game, search the downloaded 3DO titles, and review current/downloaded fields
before **Use this match**. This immediately saves available downloaded values
for the game's discs, preserves manual overrides and unavailable fields, and
remembers the LaunchBox ID. It changes information only, not disc identification,
paths or media associations. Subsequent information refreshes, missing-description
preparation and Restore downloaded use that entry. If the chosen ID becomes
unavailable, existing values are kept until you choose another match. Cached
data works offline; the match operation supports Undo.

Automatic custom-metadata snapshots are saved on the desktop before operations
using the library Undo journal, and before an explicit Undo. This includes metadata
edits/restores/imports/matches/refreshes, game imports, media preparation and
category changes. No snapshot is needed when there are no custom fields. Identical
current snapshots are not duplicated; the five newest distinct snapshots per
canonical library path are retained. These are separate from Undo and are not
copied to the console drive. **Metadata… → Automatic metadata backups…** shows
their dates, size, location and disc counts. **Review restore…** uses the normal
import review and conflict controls. Only custom information fields (including
intentional blanks) are restored; files, downloaded information, chosen LaunchBox
matches and save data are outside these snapshots. Copy a snapshot JSON from the
shown folder for a separate long-term backup. Snapshots use the same 16 MB limit
and portable format as metadata exports. Backup write failures stop the pending
operation before its journal is created.

On the console, **hold C and tap P** opens details without queuing or launching.

**Drive setup → Name / identify drive…** writes a small `.ohmy3do/drive.json`
with a persistent UUID and a friendly name, such as “Console SD.” The PC remembers
which identity used that drive letter or mount path. Copy review shows the drive
name and path, and copying, setup and boot updates recheck the ID before writing.
If another drive occupies a remembered location, check it and explicitly remember
it in Drive setup. Drive identities are never copied from the staging library.
An unnamed drive can still be prepared or copied; name it to enable identity checks.
Cloning a drive also copies its identity; rename creates a new label, not a new UUID.

Open a numbered backup in **Save Manager → Notes…** to add up to 4096 UTF-8 bytes
of notes. Slot search includes notes, which are also shown in the slot list.
They are stored alongside the archive as `slotNN.notes.txt`; copying the archive
alone does not copy its notes. Notes never change save payloads or their checksums.
An editor rejects changes if another process has updated the notes since opening.
Notes belong to the slot and remain when its archive is replaced; review them
when reusing a slot. They are managed in the desktop app.

## Categories and console visibility

Open **Settings & tools → Manage categories…** to add or rename categories,
mark any category **Hide on console**, and choose the destination for each detected
disc type. **Choose library / drive…** edits another library or USB directly.
The desktop continues to show hidden categories so you can manage and unhide them.

The shared file is `.ohmy3do/categories.json`. The eight original categories are
only defaults. Up to 32 categories are supported, with names up to 48 printable
ASCII characters; filename punctuation and reserved names are rejected.
Names must be unique ignoring case. Removing a populated category is blocked:
move its games first. Renaming updates folders, the library database, media
indexes, Favorites, Recent and saved position using one undo journal. Existing
destination folders are never merged or overwritten implicitly.

USB copying installs category names and import destinations while preserving
visibility choices already saved on that drive. Console **Tools → Settings →
Categories…** shows every configured category, including hidden ones. Press A to
show/hide one and save it immediately. This also filters Find, Favorites and Recent;
it does not delete games or backup saves. The updated boot.iso is required.

## Library and imports

Choose your staging library with **Change folder…**. The Library groups discs
under `Category/Region/Game` into one cover tile. Loose ISO/CUE files remain
independent entries. Filter by title, category or region. Click tiles to select
them; Shift-click selects a range and Ctrl+A selects all shown games.

**Add discs** opens the import workflow. Choose ISO/CUE files or folders, or
drop sources onto the app. **Scan discs** identifies releases with the existing
catalog and checksums; **Fast name-only scan** offers faster, unverified matches.
Review selected entries, categories and destinations before **Import selected**.
**Identify…** permits explicit catalog selection or an unverified title and
region. Sources remain unchanged; imported files use an undo transaction.

Selected game folders can be moved with **Move category…**. The operation
updates the database, media links, favorites, recent games, saved position and
runtime indexes. Category moves require the standard folder layout.

## Copy selected games to USB

Select tiles in Library and choose **Copy selected to USB…**. Pick a separate
USB/SD root, then **Review copy**. The preview shows the actual transfer size
and expandable file details. **Copy to USB** applies that reviewed plan.

All discs, referenced CUE tracks, covers, large-cover sidecars, previews, boot
image and required support files are included. Covers/previews are placed in
`.ohmy3do` beside their discs for the firmware's relative file access.
Find, metadata and media indexes include both selected games and games already
on the drive. Unselected games remain available, including titles no longer
in the staging library. Existing drive settings, favorites, history, saves and
custom cheat databases are preserved. No game is deleted by deselection.

Different existing disc contents block a transfer. Newly copied files pass
SHA-256 readback. Unchanged discs can use timestamps or verified-copy receipts;
**Verify existing disc contents** forces hashing. Category moves apply only to
selected games. **Cancel** rolls back a cooperative interrupted operation.

## Drive setup without games

**Drive setup** installs the menu and all required save/search support without
copying a library. Choose a drive root and **Use library boot.iso**, **Choose
boot.iso…**, or **Download released menu**. Initial settings apply only when
creating a new configuration. **Repair installed support** uses the image
already on the drive. Existing nonempty settings, backups and media are kept;
empty firmware-probe files are initialized. The occupied-slot catalog refreshes
from actual archives. The saves directory is `.ohmy3do/saves/`, not `/saves`.

For an optional PC staging library, choose its folder at the top, then use
**Settings & tools → Set up a PC staging library**. Install the menu there
before importing discs. Downloads use the published release; select a local
ISO when testing an unreleased build.

## Artwork and previews

**Artwork & previews** selects a local media folder, prepares missing media,
rebuilds existing media, or assigns a cover/video to a particular game.
FFmpeg and ffprobe download separately using a verified provider checksum.
The existing Rust encoder produces the console formats. Prepared media is
indexed for the console; PC validation does not certify hardware playback.
See the [media guide](../docs/media-guide.md) for source formats and limits.

## Save Manager

Search all occupied slots by game, raw filename or slot label. **Use USB**,
**Use library**, or **Choose folder…** selects the `.ohmy3do/saves` directory.
**Refresh** also rebuilds the console's occupied-slot catalog. Backups themselves
are authoritative; invalid nonempty archives are marked rather than hidden.

**Open backup…** validates a `.3ds` archive. **New backup** starts an empty draft.
Select an entry for its known-game controls and presets, raw hex editor or
raw/single-save export. Importing raw bytes preserves its filename and file
type. **Apply hex edits** changes only the in-memory draft. Known-game controls
maintain verified game checksums; raw edits can invalidate a game's format.

**Add saves from…** merges chosen entries from another archive into the draft,
replacing matching filenames and keeping unrelated saves. **Remove from draft**
removes one entry. **Save** writes the complete open archive; **Save as…** writes
the complete draft to a chosen slot/file. Replacing a destination slot replaces
that entire archive, not only the edited game. A verified recovery copy is
created before replacing an existing file, and outside changes to the open
archive block saving. Unsaved changes prompt before another backup is opened
or the app closes. **Compare…** shows entry and byte differences; **Name slot…**
changes the slot label without changing its save data.

See [saves](../docs/saves.md), [known save names](../docs/save-names.md), and the
[cheat database](../docs/save-cheats.md) for formats and console behavior.

## Settings and cheat database

**Settings & tools → Appearance** lets you choose Light, Dark, or System. The
choice is saved, applies immediately throughout the app, and System follows your
device’s theme automatically.

**Settings & tools** contains offline/startup-update preferences, a console INI
editor, index rebuilding, library review and undo. Software graphics can be
selected for compatibility and takes effect after restarting. Console settings retain
original bytes for conflict detection and can be undone.

**Open database editor…** edits game profiles, field addresses, widths, masks,
ranges and presets. Advanced JSON exposes signatures, checksums and multi-field
presets. Open a custom source, merge missing published profiles, save validated
JSON, or install
both JSON and compact console database into a library/drive. Bounds, overlaps,
field ranges, signatures and checksums are validated before publication. The
console keeps bundled fallback definitions; external updates need no ISO rebuild.

## App and menu updates

**Updates** checks the latest stable `desktop-v…` and `menu-v…` GitHub releases independently. The two version numbers do not need to match. New-drive setup downloads the latest stable menu rather than the desktop app’s version. Startup checks show a dismissible notice for a newer desktop version or a differing/missing boot ISO, without opening the Updates screen. **Settings & tools → Notify when updates are available** controls these notices; startup checks have their own setting. Each offer shows its own version and release notes. A boot-only offer contains only the relevant boot-image actions. Offline mode disables GitHub checks and downloads.

The Updates screen also shows the installed desktop version and the menu on a
selected library or drive. A menu version is identified by its image checksum
after a release check and remembered for offline use. Unrecognized images show
**Version unknown**; unplugged drives show **Drive unavailable**.

**Update and restart** downloads the desktop ZIP, verifies its published SHA-256
checksum and stages the exact platform executable plus allowlisted program
documentation/licenses. A helper waits for the current process to exit, checks
that both executables are unchanged, replaces the app and relaunches it. User
settings and game/save files are excluded. A recovery executable and support-file
journal are kept; a failed launch or missing frontend-ready acknowledgement
restores the previous version. Update work and logs are under the cache's
`self-update` directory. The portable app folder must be writable.

**Update on exit** downloads and verifies the same package while leaving the
app open. Close the app normally when finished; unsaved edits still receive the
usual prompts. The helper installs the update and runs a hidden startup check,
then leaves the app closed. Choose **Restart now** to install immediately, or
**Cancel scheduled update** to discard the download. Scheduling lasts for the
current session; a forced termination does not apply the update.

Update results persist in **Updates**, with the failure reason, log path and
whether the previous executable is intact. If recovery cannot be confirmed,
the app says so rather than claiming that restoration succeeded. These updates
work for both installed and portable copies without running Setup again.

For local development, open **Settings & tools → Developer updates**, enable
**Use local builds instead of GitHub releases**, and select the repository’s
`iso` folder. Use **Updates → Check for updates** after building. This works
in offline mode. It uses local packages for desktop updates and local `boot.iso`
for boot updates, drive setup and PC staging setup. Nothing is downloaded from
GitHub while developer mode is enabled.

Menu builds and desktop packaging write checksum manifests beside their output
automatically. The folder can contain either component or both. Desktop builds
are compared by executable checksum, so rebuilding without changing the version
still offers an update. Local builds can also replace a newer version. The usual
confirmation, verification and rollback remain in place. If a build changes after
checking, check again before installing it. Turn developer mode off to return to
published releases.

Boot ISO updates are separate: the checked image is replaced using the existing
verified, undoable installation path, preserving games, settings and saves. After
each boot update, the Updates screen stays open for any remaining library or
drive update.
A local unreleased build is not published automatically by building packages.

## Development

`python3 tools/build-desktop.py --test` builds Tauri and tests the shared engine.
On Linux without WebKit development libraries it uses the provided Docker
image. `--windows --test` cross-builds MSVC using cargo-xwin; under WSL it also
runs the resulting Windows test executables. `node desktop/tests/ui-model.test.mjs`
runs the frontend selection/search tests. Frontend files are embedded directly
from `desktop/ui`; there is no Node runtime or npm dependency tree to ship.

`desktop/shell` owns the Tauri window, command bridge and locked dependencies;
`desktop/src` contains the Rust engine. The previous egui shell remains behind
`--legacy-ui` for development comparisons and is not the default packaged UI.
Command-line shortcuts include `--library PATH`, `--guided-setup`,
`--save-backup PATH.3ds` and `--save-slots PATH`. `--software-rendering` selects software graphics before creating the window.
The legacy automatic screenshot switch belongs to the previous interface.

### Building installers

First build and package the Windows/Linux desktop app with
`tools/build-desktop.py` and `tools/package-desktop.py`. The installer builder
requires those complete, licensed `build/desktop-Windows-x64` and
`build/desktop-Linux-x64` payloads; it checks their executable hashes and retains
their `BUILD.json` source revision. It does not build the application itself.

```sh
docker build -t oh3do-installers -f tools/installer.Dockerfile .
mkdir -p build/installer-cache
curl --fail --location --proto '=https' --proto-redir '=https' --output build/installer-cache/MicrosoftEdgeWebview2Setup.exe 'https://go.microsoft.com/fwlink/p/?LinkId=2124703'
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src oh3do-installers python3 tools/package-installers.py --webview-bootstrapper build/installer-cache/MicrosoftEdgeWebview2Setup.exe
```

Outputs are `iso/*-Setup.exe`, `iso/*-Install.run` and their `.sha256` files.
`--platform Windows-x64` or `--platform Linux-x64` builds just one installer.
Windows packaging uses NSIS, includes its notices and records the bootstrapper
hash in `build/installer-Windows-x64/BUILD.json`. Linux's self-extracting archive
embeds the complete package and verifies its compressed payload before extracting.
An installer is an additional download; portable ZIPs remain required for the
desktop app's self-update protocol.

Native interaction and updater-recovery smoke tests are in
`tools/test-desktop-ui.py` and `tools/test-desktop-update.py`. They run in the
provided Linux image under D-Bus/Xvfb and use disposable build-directory fixtures.
