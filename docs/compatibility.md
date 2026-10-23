# Confirmed working setups

This list records what was actually tried on each setup. A result applies to
those tests, not every feature or every device with the same name.

## Console setup

| Part | Recorded setup |
| --- | --- |
| Console | Panasonic 3DO FZ-1 |
| ODE | Purchased from AliExpress; exact manufacturer/model unidentified |
| Reported ODE identity | `LOCODE 1.1.0`; exact firmware build unidentified |
| Controller | 8BitDo M30 through a BlueRetro 3DO adapter |
| SD card | PNY Pro Elite, 512 GB |
| USB reader | Reader supplied with a Samsung PRO Plus 512 GB microSDXC card |

The PNY card and Samsung-supplied reader were used together.

Confirmed during console testing:

| Test | Result |
| --- | --- |
| Boot the menu from the prepared drive | Passed |
| Browse folders and select games | Passed |
| Load covers and play video previews | Passed |
| View covers and previews full-screen | Passed |
| Launch a game without showing its disc folder during handoff | Passed |
| Run Diagnostics → Test return and resume browsing at the library root | Passed |
| Use the folder-navigation workaround | Passed |

Exact menu builds were not recorded for these individual hardware tests.
They should not be read as a complete test of every feature in the latest release.

**Controller quirk:** the M30/BlueRetro setup was reported to need an initial
button press before the menu responds. Its cause remains unconfirmed.

## Desktop setups

| Tested setup | Confirmed working | Version information |
| --- | --- | --- |
| Windows 11 | Desktop app operation | Every released build through 1.0.2: versions 1.0.0, 1.0.1 and 1.0.2 |
| Linux x64 development container | Native app interaction, local desktop update on exit, startup verification and failed-update rollback | Desktop 1.0.2 test builds; this is a container result, not a desktop-distribution report |

Installation, launch and uninstallation have also been confirmed on Windows 11.

## Add a tested setup

Use the [compatibility report form](https://github.com/Scyllinice/oh-my-3do/issues/new?template=compatibility_report.yml).
Include exact versions, device models, tasks that passed, and any failures or
settings needed. A partial test is useful; leave untried features out.

For reproducible failures, use the [bug report form](https://github.com/Scyllinice/oh-my-3do/issues/new?template=bug_report.yml).
See [collecting diagnostics](troubleshooting.md#collect-diagnostics) for reports or photos.

[Project overview](../README.md)
