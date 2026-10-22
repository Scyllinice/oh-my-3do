# Build and test from source

Run these commands from the repository root. Downloadable console images,
desktop apps and media tools are available from [Releases](https://github.com/Scyllinice/oh-my-3do/releases); building is optional. Menu and desktop versions are independent. See [the release guide](releasing.md) for packaging each stream.

## Build

Docker must be running. The first build downloads the SDK and tools. The default
SDK revision is pinned to `4e370299bd273bb13089d41fa33c5e863dc6d405`.

```sh
./tools/dev-container build
./tools/dev-container test
./tools/dev-container run make test-sdk test-covers
./tools/dev-container menu
./tools/dev-container demo
```

Outputs in `iso/`:

| Output | Purpose |
| --- | --- |
| `boot.iso` | Browser and launcher |
| `menu-demo.iso` | Offline UI demo; does not launch games |
| `*.iso.sha256` | Image checksums |
| `ohmy3do.ini` | Editable startup configuration |
| `favorites.txt` | Editable favorites template |
| `covers/` | Raw PNGs or converted thumbnails and checked index |
| `previews/` | Optional converted video/audio clips and checked index |
| `saves/` | Copy this folder into USB `.ohmy3do/` for save backups |
| `library.3di` | Search index generated on the PC by `index-media.py` |
| `metadata.3di` | Genre, publisher, and release date exported from the library database |

Builds preserve existing `iso/ohmy3do.ini` and `iso/favorites.txt`. Source
examples are `ohmy3do.ini` and `favorites.example.txt`. Build audits and signed
LaunchMe executables are retained in `build/console-menu/` and
`build/console-demo/`. The ISO includes the complete pinned SDK bootstrap
runtime and its startup scripts. Each build checks every template file before
adding the menu and artwork, and records component hashes in
`build/console-menu/runtime-audit.json`.

The container targets `linux/amd64` because the legacy compilers are x86
binaries. It includes Norcroft, Python, Wine, ImageMagick and `3it`, and mounts
the workspace with your host UID/GID. Docker's socket and physical devices are
not mounted.

## Testing and repository setup

Portable host tests require a C compiler, Make, Python 3.9+ and Pillow.
Video conversion tests also need FFmpeg and ffprobe on PATH:

```sh
make test
```

Optional firmware-function tests and actual-draw layout previews need pinned
reference sources, downloaded separately into ignored `third_party/`:

```sh
./tools/fetch-references
make test-firmware
python3 tools/preview-menu.py
```

The firmware reference test reproduces the original folder-ID bug and checks
navigation against both the original and patched implementations. Host tests
cover sorting, queues, disc grouping, favorites, recent games, settings, media
loading, USB failures, and save backup and restore.

Preview files in `build/menu-preview-*` render the actual draw/input routines
with host graphics stubs and the bundled bitmap font. These are layout previews;
console testing is still needed to check hardware rendering and behavior.

SDK checks verify the formatter ABI and inspect ARM stack allocations against
the 32 KiB task stack. ISO builds preserve the complete bootstrap runtime,
compare packed payloads with their sources, and verify both the ISO and the
retail LaunchMe signature. Build audits record source and tool hashes.

GitHub Actions is disabled. Tests and builds run locally, with no hosted
workflow runners, artifact storage, or scheduled CI jobs. `.gitignore` excludes
generated builds, reference checkouts, personal artwork, game images, saves,
caches, and credentials. `.dockerignore` limits the build context to the
required source, vendored dependencies, tests, configuration, and build tools.

See [CONTRIBUTING.md](../CONTRIBUTING.md), [LICENSE](../LICENSE),
[THIRD_PARTY.md](../THIRD_PARTY.md) and [asset provenance](../assets/README.md).
Dependencies retain their own terms. This project is a new menu implementation;
it does not patch the original compiled Fixel menu.

## Desktop builds

See the [desktop build instructions](desktop-development.md)
for Rust requirements, native builds and Windows cross-compilation.

## Emulator captures and decoder timing

See [Testing without a console](emulator-and-bench.md) for Opera setup,
screenshot scripts, ARM60 timing measurements and their limitations.

[Project overview](../README.md)
