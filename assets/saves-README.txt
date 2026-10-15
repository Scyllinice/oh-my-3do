OH MY 3DO - USB SAVE BACKUPS

Copy this entire saves folder to the root of the ODE USB drive.
Keep your working ohmy3do.ini compatibility setting beside boot.iso.

On the console, press L+R together to open Tools.
First view Internal saves, then make and verify a backup.
In Internal saves, A backs up one save and C backs up all saves.
Choose a slot and confirm; an existing slot is replaced.
The menu automatically checks USB writing before the first backup or restore
in a session. There is no standalone USB Write Test option.

slot01.3ds ... slot08.3ds are custom, checksummed file archives.
USB backups / restore opens a slot and restores selected files.
These are not raw NVRAM images or emulator .srm files.

Before restore, the menu writes and verifies all current internal saves to
recovery.3ds (or recovery2.3ds when restoring from recovery.3ds).
Both recovery archives can be opened from the menu. Copy backups to your PC.

Keep power on until finished. A restore is not atomic; an interrupted restore
can damage the selected save. Do not treat a failed operation as successful.
The USB test and readback checks do not guarantee safety against power loss.

usb-test.bin is disposable test data; it may be deleted on a PC.
Reading a missing backup slot may create an empty file on existing firmware.
An empty file is not a valid backup.

This version needs hardware testing. Start with viewing, then make a backup
and copy it to your PC before attempting a restore.
