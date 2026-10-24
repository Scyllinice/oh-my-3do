# Desktop development

Run commands from the repository root. For app usage, see the
[desktop guide](../desktop/README.md).

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

## Building installers

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

## Accessibility checks

`tools/test-desktop-accessibility.py` runs the real frontend in WebKitGTK with a
mock native bridge and disposable fixtures. It checks keyboard focus, dialog
cancellation, control labels, text contrast and reflow at 100–200% text size.
Run it in the desktop build image under D-Bus and a virtual display:

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src -e HOME=/tmp -e DISPLAY=:99 -e LIBGL_ALWAYS_SOFTWARE=1 -e WEBKIT_DISABLE_DMABUF_RENDERER=1 oh3do-desktop-tauri sh -c 'Xvfb :99 -screen 0 1600x1200x24 -ac >/tmp/display.log 2>&1 & sleep 1; exec dbus-run-session -- python3 tools/test-desktop-accessibility.py'
```

Results and screenshots are under `build/desktop-accessibility/`. This browser
test complements the native app smoke test; it does not verify Windows Narrator
output, Windows display scaling or operating-system file dialogs. Check those
on Windows before claiming assistive-technology compatibility.
