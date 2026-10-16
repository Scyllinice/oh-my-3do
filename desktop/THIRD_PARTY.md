# Desktop application dependencies

The desktop application's own code is MIT licensed. It uses Rust crates,
including eframe/egui, rfd, image, reqwest, rusqlite, compression, hashing and
platform support libraries. SQLite is compiled into the executable through
libsqlite3-sys. Bundled fonts retain their respective font licenses.

Portable packages include `licenses/dependencies.json` with the resolved crate
versions and license expressions, plus license/copyright texts collected from
the exact dependency source directories used for the build. These notices are
part of the distribution and should be kept alongside the executable.

The desktop app does not contain Python, the 3DO SDK, console runtime files,
game images, commercial covers, gameplay footage or FFmpeg binaries. Catalogs
and optional media are fetched at runtime from their providers. Video tools
are obtained separately from a provider linked by FFmpeg's download page;
the provider's licenses and documentation remain in the downloaded cache.

Provider/source references:

- Rust dependency source versions: `Cargo.lock` and packaged dependency notices.
- SQLite: https://www.sqlite.org/copyright.html
- FFmpeg: https://ffmpeg.org/download.html
- BtbN builds and build recipes: https://github.com/BtbN/FFmpeg-Builds
- Redump DAT: https://redump.info/datfile/3do/
- LaunchBox metadata: https://gamesdb.launchbox-app.com/Metadata.zip
- Cover collection: https://github.com/libretro-thumbnails/The_3DO_Company_-_3DO

The separate console ISO and Python media-tools packages have their own
project notices in the repository's main `THIRD_PARTY.md`.
