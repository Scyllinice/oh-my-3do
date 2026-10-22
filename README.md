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

1. Back up the drive's existing `boot.iso`, then replace it with the download.
2. On a fresh drive, create `.ohmy3do/` and copy the supplied `ohmy3do.ini` into it.
   Preserve an existing INI when updating. Games can stay in their current folders.
3. Disconnect the prepared drive from the PC, install it with the console powered
   off, then cold boot the console.

The optional desktop app runs without Python or Docker. It can organize discs,
prepare covers and previews, edit console settings and save backups, and prepare a USB drive.
Browse PC and drive libraries, copy selected games, or remove titles after a
file review.
It also manages game descriptions and metadata, with bulk editing, portable
custom-edit backups and a review for unmatched games.
It also offers verified updates for the app and `boot.iso`.
See [installation and drive layout](docs/installation.md) or the
[desktop guide](desktop/README.md) for the full workflow.

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
| Building, tests and emulator setup | [Build guide](docs/building.md) · [Emulator and timing](docs/emulator-and-bench.md) |
| Binary media layouts | [Format reference](docs/media-format.md) |

See [Contributing](CONTRIBUTING.md), [License](LICENSE),
[third-party notices](THIRD_PARTY.md) and [asset provenance](assets/README.md).
