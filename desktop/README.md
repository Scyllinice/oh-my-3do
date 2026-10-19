# Oh My 3DO Library

A native Rust desktop app for adding discs, finding artwork and previews, and
preparing an Oh My 3DO USB drive. The GUI, scanner, importer, media containers,
index writer and copy engine run in Rust. No Python installation, bundled
Python worker, Docker installation or Rust toolchain is needed to run a build.

## Running

Extract the portable download and launch `OhMy3DOLibrary.exe` on Windows, or
`OhMy3DOLibrary` on Linux. Keep the accompanying license notices. Windows builds
use a statically linked C runtime and support Windows 10/11 x64. Linux builds
need a normal desktop graphics stack; see the packaged build information for
the minimum glibc version. The window and Windows executable use the matching
disc icon; `OhMy3DOLibrary.png` is included for Linux desktop shortcuts.
The source also supports macOS, but a macOS build
must be made and tested on a Mac before a prebuilt app is offered.

Settings, catalogs, conversion caches, reports and undo journals are stored in
the platform cache directory under `oh-my-3do`, outside the library. Existing
catalogs from the Python organizer can be reused. The CLI tools remain
available separately.

## Library

The app uses the console menu's look: its colours, Ark Pixel for headings and
channel tabs (CH1 Library through CH5 Tools), and Atkinson Hyperlegible for
lists. **CH1 Library** shows the chosen library as a cover grid, one tile per
game folder, filtered by category, region and title. Click covers to select
them (Shift-click selects a range), choose a category in the bar at the bottom,
and **Move**. Whole `Category/Region/Game` folders move, so multi-disc sets stay
together; the database, cover and preview links, favorites, recent games, saved
position, Find and save names are updated in one undo journal. The `Adult`
category exists for adult titles, and **Review → Possibly adult** lists the
Vivid Interactive, gravure and strip-mahjong discs found in the library for you
to check before moving them. The console can hide that category
(`hide_adult` in Tools → Settings).

## Adding games

1. Choose the destination library folder. An existing staging library such as
   `D:\3DO` works; an empty folder also works. Support files live in the
   library's `.ohmy3do` folder.
2. Add ISO/CUE files or directories. Directories are searched recursively;
   support directories and redundant single-ISO CUE wrappers are skipped.
3. Select **Scan selection**. A first online scan downloads the Redump catalog
   and LaunchBox metadata; later scans use the cache. Offline mode is available
   under Tools & settings. Normal scanning verifies checksums and reconstructs
   MODE1/2048 sectors when the catalog contains the corresponding raw-track
   size. Quick scanning uses names and requires explicit acceptance.
4. Review identification, category and destination. Complete checksum matches
   with a resolved category are checked initially. Name matches, partial data
   matches, uncertain categories and ambiguous variants need a decision.
   **Choose release / edit entry** can select an exact release or register an
   explicitly unverified title. Manual choices do not become checksum matches.
5. Provide a cover or video for an entry if desired, then choose **Import
   checked**. Files are copied into `<category>/<region>/<game>/`. Discs from
   one multi-disc release share a folder, even when the sources were separate.

Source files are preserved. Archives disguised as disc images are rejected.
Conflicting destinations and shared CUE data files are refused. CUE tracks
remain together and FILE directives are updated. BIN/CUE imports retain their
track format; this version does not convert raw BIN tracks to cooked ISO files.
Audio-only discs are kept out of automatic game identification.

## Covers and previews

Set the EmuMovies download directory on **Covers & previews**. It can point at
`D:\EmuMovies` or its `Panasonic_3DO` directory. Front covers come from `Box`.
Video selection prefers `Video_MP4_HD`, then `Video_MP4_HI_QUAL`, then
`Video_MP4`, once per exact title. Previews prefer the matching region, then
fall back to a unique clip of the same game from another region. Disc numbers
and demo/full-game labels must agree. Covers stay region-specific, and
ambiguous matches are left alone. Files can also be provided for a selected import or an existing
library entry. Supplied media is converted locally.

Missing covers can be downloaded from the Libretro thumbnail collection when
a compatible, unique match is available. Previews come from local footage;
the app does not log into EmuMovies or download membership videos. Downloaded
game artwork and footage are not included in the app download.

**Update missing media** preserves existing associations. **Rebuild media quality**
reconverts recorded original images and videos for the larger console panel.
Use the matching menu from the same release. Missing originals are skipped, prior media files
are retained, and the changes can be undone. **Apply supplied
media** deliberately replaces the association for the selected entry while
retaining the former media file. **Artwork review page** produces a searchable
HTML contact sheet. Library identity and registration fields are preserved.

Covers are compressed native 96×120 RGB555 files with 192×240 companions for
full-screen viewing. Previews are 160×120 at 30 fps, capped at 15 seconds, with
mono source audio when present, in OM3DVID5 with a 1,400 KiB payload budget.
The console also needs memory for frame buffers, decoder state and allocation
guards, and can shorten a clip to fit available memory.
Enable video previews in the console configuration; existing console settings
are preserved.

Imports and **Rebuild Find index** also create `.ohmy3do/metadata.3di` from the
library database. The console menu uses it to show the selected
game's genre, publisher and release date. A year is shown when no full date
is available. The optional index is included in USB preparation.

Gecko TV is the console menu's interface. Unknown settings are rejected.

Graphics uses a working GPU when available, with software fallback if device
creation fails. **Tools & settings** shows the active adapter and offers a
software graphics override; restart after changing it. The compatibility
override is also available as `--software-rendering`. Large lists draw only
visible rows, and cover thumbnails load in the background. Graphics startup
diagnostics are saved as `native-graphics.log` in the app data folder.

Video conversion uses FFmpeg and ffprobe as separate native executables. On
Windows x64, **Get video tools** downloads BtbN's LGPL shared build directly
from its provider and checks its published SHA-256 digest. No administrator
installation or PATH editing is needed. A folder containing existing native
FFmpeg and ffprobe executables can be selected on any platform. Automatic
converter installation is currently Windows-only. The app itself is usable
offline once required catalogs and tools are cached.

## USB preparation and recovery

Use **Tools & settings → Install menu boot.iso** to add a downloaded menu ISO
to a new staging library. This copies the menu image; it does not build or
modify the console executable. **Rebuild Find index** refreshes the search
index without reidentifying games.

Choose a separate destination on **Prepare USB**, then validate and review
the copy. Copying includes disc data, boot image, settings, indexes, referenced
media and available favorites/history/saves. PC databases, reports and recovery
files stay off the USB drive. Existing destination settings and saves are
preserved; different existing disc images block the operation. A game moved to
another category in staging is moved on the drive too, when the drive holds the
same Region/Game folder under one other category, and the drive's favorites,
recent games and saved position follow it. Large covers (`-large.3dc`) and
`save-names.txt` are copied with the other media and indexes. Nothing is
deleted merely because it is absent from staging.

Each new copy passes SHA-256 readback before publication. Existing discs use
size/timestamp or saved verified-copy receipts for fast skips; **Verify all
existing disc contents** forces full hashing. Boot/header checks, config
syntax, CUE references, index CRCs, PNG chunks, and preview frame-table bounds
and payload checksums are validated. Compressed preview packets are decoded
and checked by the console as they play. PC validation cannot certify console
playback or identify a game whose contents were never matched.

**Cancel safely** stops cooperatively and restores an interrupted transaction.
**Undo last change**, or **Open undo journal**, restores the previous state.
Undo checks every affected file before changing anything and refuses to
replace files edited since the operation. Keep the journal if recovery needs
attention. Originals outside the library are never removed.

## App and menu updates

The app checks GitHub's latest stable release in the background at startup.
The window opens only when a newer desktop release is available or a configured
library/USB folder needs the released `boot.iso`. A boot-only offer explains that
the image differs (or is missing), and shows only boot downloads/install actions.
A desktop-only offer shows only the app update. If both need updating, it shows
both; folders already holding the released image are omitted. No window opens
when everything checked is current.

**Tools → Updates** has **Check for updates**, **View update details…** for pending
updates, and **Check another library / USB folder…** to add a destination.
Startup checks can be disabled. Offline mode disables checks and downloads.

The update window links to the release notes and offers the relevant desktop ZIP
and/or `boot.iso` downloads. Both are checked against the
release's published SHA-256 files; existing download files are preserved.
Close the desktop app, extract the downloaded ZIP and replace the portable app
to update it. Library files and desktop preferences stay in their own locations.

For the menu, either download `boot.iso` to a new file or install it directly
into a displayed library/USB root. **Check another library / USB folder…** adds
a destination to the check. A direct install changes only `boot.iso`, verifies
the written image and creates an undo journal. Settings, indexes, media and saves
are preserved. A file changed since the check requires another check before
installation. Finish by disconnecting the prepared drive from the PC, reinstalling
it with the console powered off, then cold booting. Use **Undo last change** to
restore the previous image if needed.

The app also accepts `--check-updates` to check at startup; the window opens only
if a desktop or boot image update is available.

## Guided setup and save editing

**Prepare USB → Guided setup for a new library / drive** installs the menu
release matching this app, verifies its published SHA-256 checksum and creates
missing settings, indexes and the saves directory. Choose an existing staging
folder, install the menu, import discs, then review the copy to a separate USB
destination. Existing settings are preserved. See the [setup guide](../docs/installation.md#pc-assisted-setup).

**Tools → Edit / update cheat database…** edits profiles, offsets, ranges and
presets. Install to a library or USB root to write `save-cheats.json` and the
console's compact `save-cheats.3db` inside `.ohmy3do`. Existing custom definitions
are preserved by USB preparation. Download official records, review merges and
install database updates without replacing `boot.iso`; restart the console to
load them. See the [database guide](../docs/save-cheats.md).

**Tools → Save Manager…** uses the shared verified save-cheat database for known
fields, offsets, presets and game checksums (currently DOOM). It opens `.3ds` archives, exports individual
saves and supports raw hex editing and a verified DOOM 3DO preferences editor.
Use **Game editor** or **Hex editor** to choose the editing view for a supported
save. **Save** updates the open archive; **Save as…** switches to the chosen file.
Existing files can be replaced after keeping a recovery copy beside them, and
archive checksums are regenerated. Open another backup at any time, with
Save/Discard/Cancel choices for unfinished edits. See the [save editing guide](../docs/saves.md#desktop-backup-browser-and-editors)
for supported fields and the console restore workflow.

The app also accepts `--guided-setup` and `--save-backup PATH.3ds` at startup.

## Console settings

**CH5 Tools → Console settings** edits `.ohmy3do/ohmy3do.ini` in a selected
library or USB root folder. Use **Use library folder**, **Use USB destination**,
or browse to another root, then **Load / reload settings**. The library's
settings load automatically when you first open Tools.

The controls cover showing covers, video previews and their sound, screen margin
(0–12 pixels), larger titles, remembered position, recent games, adult filtering,
and the folder compatibility workaround (Automatic for FCare, Always, or Off).
These controls are separate from the app's **Desktop preferences**.

**Restore defaults** changes the form; **Save console settings** writes and
verifies the INI. If no INI exists, loading uses the menu defaults and saving
creates it. Changes apply the next time you start the console with this drive.
Invalid settings are reported, and a file changed since loading must be reloaded
before saving.
**Undo settings save** restores the previous file, including its original
comments and padding; it removes a newly created INI. The normal undo journal
also remains available after restarting the app.

Preparing USB preserves an INI already on the drive. To change a prepared
drive's settings, load that drive's root here and save directly to it.

## Building and checking

Install the stable Rust toolchain and the platform's normal linker/development
libraries. The Windows MSVC build requires Microsoft's C++ build tools.

```sh
cd desktop
cargo test --locked
cargo build --release --locked
```

From the repository root on Linux or WSL with Docker,
`python3 tools/build-desktop.py --windows --test`
cross-compiles the Windows MSVC build with cargo-xwin, which downloads (and
accepts the license for) Microsoft's CRT and Windows SDK. With WSL interop
enabled, the test executables run directly on Windows. Other Linux hosts compile
the tests and print the executable paths for running on Windows.

The Windows target configuration links the C runtime statically. The Rust
backend has no dependency on the legacy Python tools. `examples/audit.rs`
checks an existing staging library without changing it; the cover/video smoke
examples create synthetic media for the console's C decoder tests.

Packaging is performed locally with `tools/package-desktop.py` (Python is used
only by this build-time packaging script). Dependency license texts and a
manifest are included with each portable download. GitHub Actions stays
disabled.

Prepared covers use compressed native 96×120 RGB555 `.3dc` files; raw PNG
covers remain supported. Playback stays entirely in RAM.

[Project overview](https://github.com/Scyllinice/oh-my-3do) ·
[Console menu guide](https://github.com/Scyllinice/oh-my-3do/blob/main/docs/menu-guide.md)
