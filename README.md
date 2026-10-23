# Oh My 3DO

![Oh My 3DO logo](assets/logo-menu.png)

An alphabetical game menu for the [FCare/SataTo3DO ODE](https://github.com/FCare/SataTo3DO),
with covers, video previews with sound, favorites, disc queues and save backups.
No firmware update is required.

![Game browser with cover artwork](docs/images/browser.png)

Actual menu v1.0.1 output captured in the Opera emulator, enlarged 3× for readability.
[More screenshots](docs/screenshots.md).

## Download and install

Get the menu/support files and desktop app from [Releases](https://github.com/Scyllinice/oh-my-3do/releases). Menu releases use `menu-v…`; desktop releases use `desktop-v…`. Their versions advance independently.

| Download | Purpose |
| --- | --- |
| `boot.iso` | Console menu |
| Windows setup EXE or Linux installer | Install the desktop app and application-menu shortcuts |
| Windows or Linux desktop ZIP | Portable desktop app; no installation needed |
| `media-tools.zip` | Optional Python command-line tools |
| `menu-demo.iso` | Offline interface demo; does not launch games |

1. Install the desktop app and use **Drive setup** to prepare your USB/SD drive.
   No games need to be copied for initial setup.
2. Add games through the Library and choose **Copy selected to USB…**.
3. Safely eject, install the drive with console power off, and cold boot.

The desktop app also manages covers, previews, descriptions, console settings,
save backups and updates. See [drive setup](docs/installation.md) for manual
installation, or the [desktop guide](desktop/README.md) for app instructions.

## Basic controls

| Control | Action |
| --- | --- |
| Up / Down | Move selection; hold to repeat |
| Left / Right or L / R | Page through the list |
| A | Open a folder or play a game |
| B | Go back |
| C | Add the selected game to the disc queue |
| Tap P (Start) | Review the queue |
| Hold P for 750 ms | Show controls for the current screen |
| Hold C + tap P | Show the selected game's information and description |
| X | Tap to switch cover/video; hold for full-screen viewing |
| L + R | Open Tools |

The [menu guide](docs/menu-guide.md) covers letter navigation, multi-disc games,
favorites, search, settings and the other shortcuts. Prepared video previews
play by default on new setups. Change this in Tools → Settings, save and cold
boot; existing settings are preserved.

## Guides

| Task | Documentation |
| --- | --- |
| Installation and support files | [Drive setup](docs/installation.md) |
| Browsing, queues, favorites and settings | [Menu guide](docs/menu-guide.md) |
| Importing games and preparing USB with the desktop app | [Desktop guide](desktop/README.md) |
| Covers, previews and full-screen viewing | [Media guide](docs/media-guide.md) |
| NVRAM backup and restore | [Save manager](docs/saves.md) |
| Command-line organization and audio bonus discs | [Library organizer](docs/library-organizer.md) |
| Incremental media updates and USB copying | [Library maintenance](docs/library-maintenance.md) |
| ODE errors and recovery | [Diagnostics](docs/troubleshooting.md) |
| Tested setups and hardware quirks | [Compatibility reports](docs/compatibility.md) |
| Building, tests and emulator setup | [Build guide](docs/building.md) · [Emulator and timing](docs/emulator-and-bench.md) |
| Binary media layouts | [Format reference](docs/media-format.md) |

To report a problem or share a working setup, use the
[issue forms](https://github.com/Scyllinice/oh-my-3do/issues/new/choose).

See [Contributing](CONTRIBUTING.md), [License](LICENSE),
[third-party notices](THIRD_PARTY.md) and [asset provenance](assets/README.md).
