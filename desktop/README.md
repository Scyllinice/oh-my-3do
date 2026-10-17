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
the minimum glibc version. The source also supports macOS, but a macOS build
must be made and tested on a Mac before a prebuilt app is offered.

Settings, catalogs, conversion caches, reports and undo journals are stored in
the platform cache directory under `oh-my-3do`, outside the library. Existing
catalogs from the Python organizer can be reused. The CLI tools remain
available separately.

## Adding games

1. Choose the destination library folder. An existing staging library such as
   `D:\3DO` works; an empty folder also works. Existing libraries must use the
   consolidated `.ohmy3do` support layout.
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

**Update missing media** preserves existing associations. **Apply supplied
media** deliberately replaces the association for the selected entry while
retaining the former media file. **Artwork review page** produces a searchable
HTML contact sheet. Library identity and registration fields are preserved.

Covers are 84×105 PNGs. Previews are 15 fps, capped at 15 seconds, with mono
source audio when present; complex clips shorten further to fit the menu's
1 MiB RAM payload budget. Playback requires the v0.16.0-rc2 or newer menu. Enable video previews in the
console configuration; existing console settings are preserved.

Imports and **Rebuild Find index** also create `.ohmy3do/metadata.3di` from the
library database. The v0.16.0-rc5 console menu uses it to show the selected
game's genre, publisher and release date. A year is shown when no full date
is available. The optional index is included in USB preparation.

Desktop v0.1.2 accepts and copies Gecko TV settings (`theme=3`), introduced
in console v0.16.0-rc6. Older desktop builds reject this new theme value.

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
preserved; different existing disc images block the operation. Nothing is
deleted merely because it is absent from staging.

Each new copy passes SHA-256 readback before publication. Existing discs use
size/timestamp or saved verified-copy receipts for fast skips; **Verify all
existing disc contents** forces full hashing. Boot/header checks, config
syntax, CUE references, index CRCs, PNG chunks and every compressed preview
frame are validated. Validation cannot certify console playback or identify a
game whose contents were never matched.

**Cancel safely** stops cooperatively and restores an interrupted transaction.
**Undo last change**, or **Open undo journal**, restores the previous state.
Undo checks every affected file before changing anything and refuses to
replace files edited since the operation. Keep the journal if recovery needs
attention. Originals outside the library are never removed.

## Building and checking

Install the stable Rust toolchain and the platform's normal linker/development
libraries. The Windows MSVC build requires Microsoft's C++ build tools.

```sh
cd desktop
cargo test --locked
cargo build --release --locked
```

The Windows target configuration links the C runtime statically. The Rust
backend has no dependency on the legacy Python tools. `examples/audit.rs`
checks an existing staging library without changing it; the cover/video smoke
examples create synthetic media for the console's C decoder tests.

Packaging is performed locally with `tools/package-desktop.py` (Python is used
only by this build-time packaging script). Dependency license texts and a
manifest are included with each portable download. GitHub Actions stays
disabled.
