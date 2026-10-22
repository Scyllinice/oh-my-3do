# Troubleshooting

## Missing support files or backups folder

In the desktop app, choose **Drive setup → Repair installed support** for the
drive containing `boot.iso`. Saves belong in `.ohmy3do/saves/`, not `/saves`.
Use **Check drive** afterward for any remaining problems.

## Covers, previews or descriptions do not appear

1. Check **Tools → Settings** for Show covers and Video previews.
2. In the desktop app, prepare missing media in the PC library.
3. View the console drive and use **Update media & information…**.
4. Run **Check drive**, then safely eject and cold boot the console.

After manual file changes, rebuild the indexes too. See the [media guide](media-guide.md).

## Folder navigation fails

Leave **Folder workaround** on **Auto** and keep files named
`Oh My 3DO - Folder Navigation Helper (Keep).iso`. If the menu says
**No safe folder workaround here**, repair the drive with desktop **Drive setup**.

If a failed media load leaves the menu in the wrong folder, use **B: Return to
library** when offered. Successful recovery disables media and position saving
until restart; it does not change settings on the drive. If recovery fails or
the ODE times out, power-cycle the console.

For startup problems, **hold X while booting** to bypass optional USB settings
and media. Check the SD card, reader and any extender if the console does not
recognize the disc at all.

## Restore failed

Keep the recovery backup. Do not assume the selected save was restored.
Copy the slot and recovery archives to a PC before making further changes.
Check the drive and available console save space, and record the diagnostics
before retrying. See the [Save Manager guide](saves.md).

## Safely eject failed

Close files and other programs using the drive, then try the operating system's
eject controls. Some readers do not support ejecting one card independently.
Keep the app and its cache off the drive being removed.

## Collect diagnostics

Open **L+R → Diagnostics** on the console. Use Left/Right to view the connection,
settings and last media operation. After a failure, note the last requested
filename and fault message or take a photo.

Press **C** to export `.ohmy3do/diagnostics.txt` when export is available. A hung
connection may prevent export even though the diagnostics screen still works.

In the desktop app, **Drive setup → Export support report…** combines drive
checks, console diagnostics and recent app activity. Review the report before
sharing: it includes paths, but no game or save contents.

For protocol and firmware details, see the [firmware reference](../firmware/README.md).
For builds and test procedures, see [building](building.md) and
[emulator testing](emulator-and-bench.md).

[Project overview](../README.md)
