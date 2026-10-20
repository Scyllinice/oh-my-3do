OH MY 3DO - USB SAVE BACKUPS

Copy this entire saves folder into .ohmy3do/ on the ODE USB drive, so backups
live in .ohmy3do/saves/. Keep ohmy3do.ini in .ohmy3do/ and boot.iso at the USB root.

On the console, press L+R together to open Tools.
First open Save manager, then make and verify a backup.
In Save manager, A prepares one save and C prepares all saves for backup.
Choose a slot and confirm. A single save merges into the slot; C:All replaces it.
L/R changes pages of eight slots; P opens Find slots (occupied/search/jump/all).
Find saves across slots in the desktop Save Manager refreshes the console catalog.
Run it once for old backups and again after manually copying/deleting backups.
Console searches match game names, raw filenames and slot labels.
Jump accepts slot numbers 1-999999999.
USB capacity and filesystem/firmware limits determine actual storage capacity.
Inside a USB archive, P copies the selected save into another slot.
In the slot list, C names a slot and X compares it with another slot.
The menu automatically checks USB writing before the first backup or restore
in a session. There is no standalone USB Write Test option.

slot01.3ds, slot09.3ds, slot1000.3ds etc. are custom, checksummed archives.
Labels are stored separately in slot-labels.txt and slot-labels-N.txt (page N).
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
