# Maintain a library and prepare USB

Use the [desktop app](../desktop/README.md) for a graphical workflow. The commands
below are the optional Python alternative for updating an organized staging
library and copying its runtime files to a USB drive.

`library-maintenance.py` adds missing media to an organized library without a
new identification scan, and prepares a USB copy containing runtime files.
Run these commands from the extracted `media-tools` folder:

```powershell
py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies
py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies --apply
py library-maintenance.py review D:/3DO --report artwork-review.html
py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json
py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json --apply
```

Replace `M:/` with the USB drive's actual letter. Both operations preview by
default. The HTML media report includes cover thumbnails, title/path search,
and a missing-media filter. Cover imports/thumbnails need Pillow; preview
conversion needs FFmpeg and ffprobe. USB validation and copying need only Python.

Media updates preserve existing artwork, disc identities, registrations and
folder locations. New discs first need to be organized or registered and
indexed. The command uses the current database and never hashes game images.
EmuMovies front covers come from `Box`; videos prefer `Video_MP4_HD`, then
`Video_MP4_HI_QUAL`, then `Video_MP4`, selecting one quality per title. Exact
matches are preferred, then compatible regions. When none are available, a
unique preview of the same game from another region can be used. Covers stay
region-specific; ambiguous matches stay unselected. `--cover-dir` and `--preview-dir` select other local sources.
Conversions are cached by source content and settings; imports are undoable
with the organizer. New previews use 160×120 at 30 fps, a 15-second cap, and source audio.

USB preparation validates the `.ohmy3do` support files, the boot volume
header/length, settings, Find, disc/CUE paths and indexed media. It checks PNG
chunks and the frame tables and checksums of `OM3DVID5` previews, whose frames
the menu checks as it plays them. This does not verify the
boot signature or replace hardware testing. A stale Find index must be rebuilt
with `py index-media.py D:/3DO` before copying.

The copy includes disc files, `boot.iso`, settings, indexes, referenced media,
and available favorites/history/saves. PC databases, reports, caches and
recovery data stay on the PC. Existing destination settings, favorites,
history and saves are preserved. Different existing disc files block copying;
updated runtime files are backed up beside the copy report. Each new copy
passes SHA-256 readback before replacement. Interrupted copies can be rerun,
and destination files are never deleted.

Existing discs with the same size and exact timestamp are skipped quickly.
Reuse the same copy report to retain verified-copy receipts, which also avoid
rehashing unchanged copies when FAT rounds their timestamps. `--verify-discs` instead hashes them all, which can take considerable time.
Other runtime files are compared by content. Keep reports, conversion caches
and copy backups outside both library roots. The full command reference is
included in the media-tools ZIP's `README.txt`.

## Category moves and undo

The CLI `library-maintenance.py prepare-usb` follows category moves too. Its JSON
report records an undo `journal`; restore it with `organize-library.py undo
<USB root> <journal path relative to the USB root>`.

[Project overview](../README.md)
