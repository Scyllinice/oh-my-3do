# Publishing menu and desktop releases

The repository contains two independently versioned products. Either product can
advance on its own. The existing combined `v1.0.0` release stays intact.

| Stream | Tag | Version settings | Downloads |
| --- | --- | --- | --- |
| Console menu | `menu-vX.Y.Z` | `MENU_VERSION` in `src/main.c`; `SCANNER_VERSION` in `tools/organize-library.py`; media-tools README heading | `boot.iso`, `menu-demo.iso`, checksums, default INI, `media-tools.zip`, notices |
| Desktop app | `desktop-vX.Y.Z` | Both desktop Cargo manifests/lockfiles and `desktop/shell/tauri.conf.json` | Platform ZIPs, installers, checksums, notices |

The desktop's shared Rust engine uses the desktop version. Portable media tools
are part of the menu stream. File-format compatibility determines which products
work together; matching release numbers are not required. If a future menu needs
a new support-file format, document the minimum compatible desktop version in
that menu release.

## Build and stage

Run the relevant checks from [the build guide](building.md) and
[desktop guide](desktop-development.md). Finish and commit
changes before preparing published assets; use that commit as the package revision.

Inspect versions and expected assets without building or writing anything:

```sh
python3 tools/release.py menu
python3 tools/release.py desktop
```

For a menu release, build both ISOs and the media tools:

```sh
./tools/dev-container menu
./tools/dev-container demo
python3 tools/package-media-tools.py --require-native
(cd iso && sha256sum media-tools.zip > media-tools.zip.sha256)
```

The media package requires the native Windows/Linux codec and preview encoder
libraries; build these as described in the [desktop guide](desktop-development.md).
`iso/ohmy3do.ini` must contain the source defaults for publication. Menu builds
preserve edits to this generated file, so check it before staging.

For a desktop release, build the desired platforms, package each executable with
`tools/package-desktop.py --revision COMMIT --platform PLATFORM BINARY`, then
create its installer with `tools/package-installers.py`. The
[desktop guide](desktop-development.md) has the container commands
and prerequisites. Menu images do not need rebuilding for a desktop-only release.

Stage exactly one stream:

```sh
python3 tools/release.py menu --stage
python3 tools/release.py desktop --stage
```

Outputs are `build/releases/menu-vX.Y.Z/` and
`build/releases/desktop-vX.Y.Z/`. Desktop staging defaults to Windows and Linux;
use repeated `--platform` arguments for other selections. `--output NEW_DIRECTORY`
chooses another destination. Existing staging directories are never overwritten.
The script checks component versions, asset checksums and package versions,
and records the stream, tag, source revision and dirty state in `BUILD.json`.
Menu staging also checks the version source against both ISO build audits.
It never creates Git tags, uploads files or publishes releases.

## Publish

Use a new tag and release for each stream. Give release notes only the changes
for that component. For example, after reviewing the staged assets:

```sh
git tag -a menu-v1.0.1 COMMIT -m 'Oh My 3DO menu 1.0.1'
git push origin menu-v1.0.1
gh release create menu-v1.0.1 build/releases/menu-v1.0.1/* \
  --verify-tag --title 'Oh My 3DO Menu 1.0.1' --notes-file MENU_NOTES.md --latest=false
```

Use `desktop-v1.0.1`, the desktop staging folder and desktop notes for an app
release. Publish only the component being released. These tags can point to
different commits. Never replace a published version's files.

GitHub has one repository-wide “latest” marker, so it cannot identify both
streams. Download links in the guides point to the release list. The desktop
reads the paginated release list, ignores drafts/prereleases, and chooses the
highest numeric stable version for each stream. Before a stream's first release,
it falls back to a historical combined release. Setup and self-update use the
selected menu and desktop releases respectively; checksums and URLs stay pinned
to those exact tags. Release discovery does not depend on GitHub's “latest” marker.
