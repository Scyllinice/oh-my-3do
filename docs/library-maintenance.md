# Maintain a library and prepare USB

For the graphical workflow, see the [desktop guide](../desktop/README.md).
These commands are the optional Python alternative for an already organized
library. New games should be imported with the [library organizer](library-organizer.md) first.

Run commands from the extracted `media-tools` folder. Replace paths and drive
letters with your own. On Linux/macOS, use `python3` instead of `py`.

## Update missing media

Preview the changes, then repeat with `--apply`:

```powershell
py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies
py library-maintenance.py update-media D:/3DO --emu-movies D:/EmuMovies --apply
py library-maintenance.py review D:/3DO --report artwork-review.html
```

Existing media and game locations are preserved. Cover conversion needs Pillow;
video conversion needs FFmpeg and ffprobe. Use `--cover-dir` or `--preview-dir`
for other local media folders. See [manual media preparation](media-tools.md).

## Copy to USB

```powershell
py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json
py library-maintenance.py prepare-usb D:/3DO M:/ --report usb-copy-report.json --apply
```

Review the destination and plan before applying. The copy includes games, menu,
media and required support files. Existing destination settings, favorites,
history and saves are preserved. Conflicting game files block copying;
destination games are not deleted.

Reuse the same report for later copies to skip previously verified files.
`--verify-discs` checks all existing disc contents and takes longer. Keep reports,
conversion caches and copy backups outside both library roots.

If the search index is stale, run `py index-media.py D:/3DO` before copying.
Safely eject afterward and install the drive with console power off.

## Category moves and undo

USB preparation follows category moves. Its JSON report records an undo
`journal`; restore it with:

```powershell
py organize-library.py undo M:/ JOURNAL_PATH
```

Replace `JOURNAL_PATH` with the journal path from the report, relative to the
drive root. The media-tools ZIP's `README.txt` contains the full command reference.

[Project overview](../README.md)
