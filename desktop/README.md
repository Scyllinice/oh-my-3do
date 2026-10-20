# Oh My 3DO Library

A Tauri 2 desktop app with an HTML/CSS interface and the shared Rust library
engine. Game imports, media conversion, USB transfers, save editing and
verification run in Rust. No Python or Node installation is needed to use it.

## Install and start

Extract the complete Windows or Linux ZIP into a writable folder and start
`OhMy3DOLibrary.exe` (Windows) or `OhMy3DOLibrary` (Linux). Keep the license
files beside it. Windows requires Microsoft Edge WebView2, normally present on
Windows 10/11. Linux requires GTK 3 and WebKitGTK 4.1, for example Debian's
`libgtk-3-0` and `libwebkit2gtk-4.1-0` packages. The Linux ZIP does not bundle
those system libraries.

Preferences, catalogs, undo journals and update downloads live in the platform
cache directory under `oh-my-3do`. The app continues to use
`native-settings.json`. Changing the GUI does not change disc or save formats.

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

**Updates** checks the latest stable GitHub release. Startup checks open an offer
only for a newer desktop version or a differing/missing boot ISO on a checked
root. A boot-only offer contains only the relevant boot-image actions. Offline
mode disables checks and downloads.

**Update and restart** downloads the desktop ZIP, verifies its published SHA-256
checksum and stages the exact platform executable plus allowlisted program
documentation/licenses. A helper waits for the current process to exit, checks
that both executables are unchanged, replaces the app and relaunches it. User
settings and game/save files are excluded. A recovery executable and support-file
journal are kept; a failed launch or missing frontend-ready acknowledgement
restores the previous version. Update work and logs are under the cache's
`self-update` directory. The portable app folder must be writable.

Boot ISO updates are separate: the checked image is replaced using the existing
verified, undoable installation path, preserving games, settings and saves.
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

Native interaction and updater-recovery smoke tests are in
`tools/test-desktop-ui.py` and `tools/test-desktop-update.py`. They run in the
provided Linux image under D-Bus/Xvfb and use disposable build-directory fixtures.
