# Desktop application dependencies

The desktop application's own code is MIT licensed. It uses Rust crates,
including Tauri, Wry, Tao, rfd, image, reqwest, rusqlite, compression, hashing and
platform support libraries. SQLite is compiled into the executable through
libsqlite3-sys. Bundled fonts retain their respective font licenses: the app's
Tauri interface uses the system UI font, with bundled Atkinson Hyperlegible
(Braille Institute of America) as a fallback under the SIL Open Font License 1.1.
Ark Pixel licenses are retained for the optional legacy interface. Their license texts are in `licenses/fonts/`.

Portable packages include `licenses/dependencies.json` with the resolved crate
versions and license expressions, plus license/copyright texts collected from
the exact dependency source directories used for the build. These notices are
part of the distribution and should be kept alongside the executable.

The optional Windows setup executable uses [NSIS](https://nsis.sourceforge.io/License),
with its notices installed as `licenses/installer/NSIS.txt`. It embeds Microsoft's
Evergreen WebView2 bootstrapper, downloaded from Microsoft's official distribution
link. Setup invokes it only if the shared runtime is missing; the runtime itself
is then downloaded from Microsoft and remains installed when this app is removed.
See [Microsoft's distribution documentation](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).
The Linux per-user installer uses a shell wrapper and the same licensed desktop
payload; it does not bundle GTK or WebKitGTK.

The desktop app does not contain Python, the 3DO SDK, console runtime files,
game images, commercial covers, gameplay footage or FFmpeg binaries. Catalogs
and optional media are fetched at runtime from their providers. Video tools
are obtained separately from a provider linked by FFmpeg's download page;
the provider's licenses and documentation remain in the downloaded cache.

Provider/source references:

- Rust dependency source versions: `shell/Cargo.lock` and packaged dependency notices.
- SQLite: https://www.sqlite.org/copyright.html
- FFmpeg: https://ffmpeg.org/download.html
- BtbN builds and build recipes: https://github.com/BtbN/FFmpeg-Builds
- Redump DAT: https://redump.info/datfile/3do/
- LaunchBox metadata: https://gamesdb.launchbox-app.com/Metadata.zip
- Cover collection: https://github.com/libretro-thumbnails/The_3DO_Company_-_3DO

The separate console ISO and Python media-tools packages have their own
project notices in the repository's main `THIRD_PARTY.md`.
