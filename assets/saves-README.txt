OH MY 3DO - USB SAVE BACKUPS

Copy this entire saves folder into .ohmy3do/ on the ODE USB drive, so backups
live in .ohmy3do/saves/. Keep ohmy3do.ini in .ohmy3do/ and boot.iso at the USB root.

On the console, press L+R together to open Tools.
First open Save manager, then make and verify a backup.
In Save manager, A prepares one save and C prepares all saves for backup.
Choose a slot and confirm; an existing slot is replaced.
The menu automatically checks USB writing before the first backup or restore
in a session. There is no standalone USB Write Test option.

slot01.3ds ... slot08.3ds are custom, checksummed file archives.
USB backups opens a slot and lets you restore a selected file.
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

Start with viewing, then make a verified backup and copy it to your PC before
attempting a restore. A successful backup does not prove that a later restore
will survive power loss or an ODE fault.
