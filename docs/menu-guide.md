# Console menu guide

Browsing, disc queues, favorites, search and settings. Support-file paths below
are relative to `.ohmy3do/` at the USB root unless a full path is shown. See
[installation](installation.md) for drive setup.

Folders sort first, ignoring ASCII case and using natural numeric order:
`Disc 2` comes before `Disc 10`.

## Controls

| Control | Browser |
| --- | --- |
| Up / Down | Move selection; hold to repeat |
| Left / Right or L / R | Page through the list |
| A | Open a folder or launch a game; a disc from a multi-disc set also queues the discs after it |
| Hold C + tap P | Game information and description for the selected entry |
| Hold C + A | Launch only the selected disc; on a folder that acts as a game, open the folder |
| B | Parent folder |
| C | Add selected image to the queue when released |
| Hold C + Up | Focus the alphabet strip from anywhere in the list |
| Hold C + Left / Right | Jump to the previous/next initial letter without adding a disc |
| Tap P (Start) | Review the disc queue (on release) |
| Hold P for 750 ms | Controls for the current menu screen |
| X | Clear an active find filter; else tap to switch cover/video. Hold for 500 ms to show the current cover or playing preview full-screen; any button returns. Otherwise refresh / retry connection |
| L + R together | Tools: saves, backups, favorites, recent games, find, configuration, help, diagnostics; also cancels a cover/preview load |
| Hold X at startup | Skip USB configuration, artwork, favorites and settings writes |

Hold P in the browser or a tool to see its controls. B or P closes help and
returns to the same screen with the selection and unsaved edits preserved.
A short P press keeps its usual action and runs when released. Full-screen
media keeps its existing any-button-to-return behavior.

The browser uses a proportional, mixed-case bitmap font, a dedicated artwork
panel, square selection bars and raised, colour-coded controller prompts shaped
like the 3DO pad's buttons (round A/B/C, oval P and X, capsules for the shoulder
and direction buttons; P is the pad's Start button). Under the list, prompts
show what A and C (or Start) do for the selected entry, with `B Back` (inside a
folder or the queue) and `L+R Tools` below them. Beside the channel number, `QUEUE n` shows queued discs and `UNSAVED`
marks unsaved settings. The Tools screen has two
columns with seven cards; the arrows move between them. Cover images and video
previews share the larger artwork panel.

**Save Manager** contains **Console saves** and **USB backups** views; use
Left/Right to switch. In the USB view, L/R changes slot pages, P opens Find slots for occupied backups,
game/name search, the full numbered view, or a jump to a
slot number, C names a slot, and X compares backups. Inside a backup, P copies
one save to another slot; C opens the hex editor and X opens verified cheats.
See the [Save Manager guide](saves.md) for backup, editing, and restore controls.

Single **L/R** presses turn a page when released. Holding either shoulder and
adding the other opens Tools without paging first; release both before another
L+R shortcut.

Game lists hide `.iso` and `.cue` extensions from titles and show an ISO or CUE
label beside each entry. Both formats remain selectable, including when they
share a title. For BIN/CUE games, select the CUE; it describes the tracks and
references the companion data files. BIN files are hidden throughout the browser.
Full filenames and original TOC IDs are retained.
Game labels consistently hide recognized region and language tags, while keeping
edition, revision, disc and other designators. Identical cleaned names receive
`[1]`, `[2]`, etc. in the browser and title card; files and folders are not renamed.
Select an entry, **hold C and tap P** to open **Game details**. This view shows
available genre/publisher/date metadata and a description, followed by the
original name, location and disc filenames. Left/Right pages through the text;
B returns directly to the same browser selection. It does not queue or launch
the game. Descriptions are optional: use **Prepare missing media** or
**Get game information…** in the desktop Library, or edit them under
**Game details → Edit information…**, then copy the selected games to the console drive. They remain available
offline. The console reads one small description file on opening details, using
the same folder restoration safeguards as other USB tools. Normal scrolling
and preview playback do not load descriptions.

Long selected names scroll after a pause; folder changes reset the animation.
Parent navigation restores selection. The
previous listing remains visible while a folder loads. A read failure hides the
listing and prevents selection until the connection recovers. The whole
directory is sorted before paging. Normal text shows eleven rows; large filenames
show eight. Browsing redraws changed rows and details while retaining the TV frame
and logo in each display buffer; an unchanged screen needs no redraw. Artwork
waits until selection settles, and queued movement takes priority over preview loading.
Uncached covers wait 700 ms and previews wait 1.6 seconds after selection settles.
Letter navigation redraws only the old and new letters; held directions repeat
every 50 ms after an initial 250 ms pause.
A small gecko lounges in a recliner beside the channel readout and clicks its remote when selection changes.
Its 16-frame animation is embedded in the boot image (8 KiB of pixels), runs once
without delaying input or artwork, and leaves idle browsing free of redraws.

At a USB root, support entries are hidden: `boot.iso`, `menu-demo.iso`, their
`.sha256` files, and `.ohmy3do`.
Identically named entries inside game folders remain visible. Volume selectors
are never hidden. The compatibility workaround retains access to hidden entries
when needed.

Drive preparation adds a 2 KB `Oh My 3DO - Folder Navigation Helper (Keep).iso` helper to intermediate
library folders. It is hidden from the menu and excluded from game indexes and
imports. The ODE must see this file, so do not give it a FAT hidden attribute.
It supplies a safe navigation detour for firmware folder-ID collisions and is
never queued or launched. Running drive setup again repairs missing helpers.
Keep these files when adding more games or folders; they remain in place so
navigation still works if the library layout changes again.

## Disc queue and automatic grouping

The queue is intended for discs from **the same multi-disc game**, added in
their required order. Changing the mounted image leaves the first game's program
running; it is not a reliable way to start an unrelated game. Advance when the
game requests the next disc. The menu stops running after launching a game, so
it cannot show a live mounted-disc overlay or detect a game's swap prompt.

Press C on images to add up to 16 discs, then Start to review them:

| Control | Queue review |
| --- | --- |
| A / P | Apply the displayed order and launch |
| X | Remove the selected disc |
| C | Toggle move mode; Up/Down moves that disc, C or B finishes |
| Hold C, press X | Clear the complete queue |
| B | Return to browsing |

Queue edits happen in menu memory. Before launch, the menu validates every path,
clears the firmware queue, and re-adds discs in the displayed order. It resolves
fresh directory IDs and restores/verifies the browser folder between visits.
Same-folder queues use one fresh directory listing and add every disc in place,
avoiding repeated walks through the folder tree.
This supports discs in different folders and volumes. A missing path prevents
launch; an uncertain append/clear blocks further launch until a confirmed clear.
An XBUS timeout requires a console restart. A failed directory restoration can
offer a verified return to the library when the connection remains responsive
(see Diagnostics below). The queue is not saved across power cycles.

With an empty queue, launching any disc of a recognized set queues it together
with the discs after it and starts immediately: Disc 1 queues 1, 2, 3...; Disc 2
queues 2, 3... The firmware mounts the next queued disc each time the game asks
for one; after the last disc the queue empties and the next disc change returns
to the menu. Hold C while pressing A to launch only the selected disc.

A set is recognized from explicit `Disc`, `Disk` and `CD` markers with numbers,
including names such as `D (Disc 1).iso` and `Game CD1.cue`. All entries must be
in the **same folder**, have matching surrounding names, contain unique disc
numbers, and run continuously from 1 through N. Region/revision differences,
duplicate disc numbers, gaps, ambiguous markers and oversized sets launch the
disc alone. Files in separate Disc 1/Disc 2 subfolders must be queued manually.
The menu cannot establish that matching filenames contain compatible game data.

### Game folders

When the library's metadata index (`.ohmy3do/metadata.3di`, written by the
desktop app) shows a folder holding exactly one game, either a single disc or a
complete disc set as above, the folder lists and plays as that game: no `>`, its
cover, preview and details, `A` to play (a set queues its other discs), `C` to
queue it, and favorites save its disc. Playing keeps the browser and remembered
position on the game list. Hold C while pressing A to open the folder instead.
After a successful mount, the last launch screen stays visible until the game
takes over. Folders that contain further folders of games, or discs that
do not form one set, stay ordinary folders. Every disc is indexed for this
recognition, including titles without genre, publisher, or release-date data.

## Favorites and letter navigation

Put `favorites.txt` in `.ohmy3do/`, with one path relative to that USB volume
per line. Use forward slashes and include the actual extension. For example:

```text
# Favorites
Games/Road Rash.iso
Games/D/D (Disc 1).iso
```

Open Tools → Favorites. **A** opens the containing folder and selects the image;
press A again to launch, including the remaining discs of a recognized set.
**C** adds the game currently selected in the browser; **X** removes the highlighted favorite. Both
edits save and verify `favorites.txt` immediately, creating it if missing.
**Start** reloads the list. Select a game before opening Tools to add it.

**R** enters move mode. Up/Down moves the highlighted favorite; **A or R** saves
the new order and verifies it. **B** cancels without writing. Before saving, the
menu re-reads the USB list and checks that it still matches the list opened for
editing. If another edit is detected, cancel and reload instead of overwriting
it.

Missing paths leave the original folder intact when restoration succeeds. A
timeout requires a power cycle; a responsive connection with failed folder
restoration can offer **B: Return to library** (see Diagnostics). Files are read on
demand, not automatically at startup. Each USB volume can have its own
favorites. Do not include virtual USB device labels in favorites paths.

The list supports up to 64 entries in fewer than 8,192 bytes, with each path
under 1,024 bytes. Blank lines, `#`/`;` comment lines, CRLF/LF and a UTF-8 BOM
are accepted. Traversal components, absolute paths, backslashes, embedded
controls, duplicate paths ignoring ASCII case, and oversized lists are rejected
as a whole. Edits re-read the file first, reject invalid/unreadable lists and
case-insensitive duplicates, and write a canonical path list, replacing
comments/formatting. A NUL terminator hides old file tails because firmware
cannot truncate files. No favorites are saved without pressing an edit button.
The firmware may create an empty missing text file even for a read request.

The alphabet strip below the browser lists **# and A–Z**. Focus it with **C +
Up**, Up from the first entry, or Down from the last entry. Left/Right chooses a
letter, A jumps to its first matching visible name, and B cancels. Down returns
to the first list entry. Letters without matches are dimmed; confirming one
leaves the strip open. `#` covers numeric and other initials. The strip follows
the current name filter and never launches or queues a game.

Hold C and use Left/Right for quick jumps between available initials. A plain C
tap adds a disc only on release, so either alphabet shortcut avoids queueing.

## Remembered position, recent games and finding names

The menu records the last folder and selected entry in `last-position.txt` on
each USB volume. After two seconds without a selection change, the menu saves a
changed position and verifies it. It also saves before launch. On startup it
resolves the folder by name using fresh directory IDs and selects the saved
entry. A missing or renamed folder leaves the startup listing available. A
malformed position file disables automatic position writes for the session;
delete that file on a PC to start fresh. A failed write also stops automatic
position saves for the session. Position data supports the browser's depth and
path limits. The loading artwork stays visible until startup settings and the
saved location are ready, so the browser first appears at its saved size.

Tools → Recent games lists up to ten recent launch selections, newest first.
**A** opens the containing folder and selects the image; **Start** reloads the
list. Paths are recorded in `recent.txt` just before handing control to a game,
so a failed mount can still appear in the list. Repeating a selection moves it
to the top without duplicates. Reads and verified writes share one trip to the
USB root, and repeating the newest cached selection skips the history write.
Long paths may reduce the number of entries;
paths of 1,024 bytes or more are not recorded. Missing paths do not launch a
game, and malformed lists are not overwritten automatically.

Tools → Find a game opens a controller keyboard. Use the arrows to choose a
character, **A** to enter it, **X** to erase, **C** to clear, and **Start** to
search. `SP` enters a space. Search matches game-image names across folders on the
selected USB drive, ignoring ASCII case. An empty query lists all games.

Run the updated `index-media.py` on the PC before using Find:

```powershell
py index-media.py M:/
```

The same command creates cover/preview associations and `.ohmy3do/library.3di`, even when there is no artwork. Find loads this checked file once and
keeps it in RAM. It does not recursively scan folders on the console. Matching
runs once per submitted query; drawing and navigating results use a stored match
list. The initial index read still requires a USB-root round trip.

Results show the selected game's folder to distinguish duplicate names.
**A** opens that folder and selects the game; it does not launch automatically.
**Start** or **B** returns to the keyboard, **X** reloads the index, and Left/Right
or L/R pages through results. Regenerate the index after adding or moving games.
Search covers the selected USB volume; enter a volume before searching from a
multi-device root. The index supports 2,048 games and 128 KiB of cached paths.
Support folders and the menu ISOs are excluded. A missing or invalid index shows
setup instructions instead of starting a slow directory scan.

Both position recording and recent-game recording can be disabled in Settings.
They default to enabled. Holding X at startup skips these USB extras.
Each USB volume has its own files; startup
restoration uses the boot volume's position file. Recent paths retain volume
labels where needed. A regular history-file failure does not prevent launch,
while an uncertain transport or failed folder restoration blocks it.

## Display preferences and configuration

Tools → Settings exposes display and library preferences: additional screen
margins (0–12 pixels), larger game/queue titles, the folder compatibility workaround, Show covers, Video previews,
remembered position, recent games and preview sound. **Categories…** opens the
show/hide list for every configured category. Press A to toggle one and save it
immediately; B returns to Settings. Hidden categories are excluded from browsing,
Find, Favorites and Recent, while their files and list entries remain on USB.
Configure category names and import destinations in the desktop app; these are
shared in `.ohmy3do/categories.json`. Left/Right or A changes the selected
value. Preview sound, display and hiding changes take effect immediately. Save the remaining
options and cold boot to apply them. The settings list spans two pages; keep
pressing Down to reach the remaining actions, or press C to save from either page.

The menu uses one Gex-inspired look, **Gecko TV**: a charcoal remote-control
list with lime selection keys, and the cover or video preview on the screen of a
charcoal portable TV, under a green channel display. Changing to a game with a
preview shows static and a tuning bar on the TV's screen while the clip loads;
the bar appears only while something is loading. There is no theme setting;
unknown INI keys such as `theme` are rejected.

![Gecko TV root browser](images/gecko-tv.png)

Screenshot of the v1.0.0 boot image in Opera: native 320×240, enlarged to 960×720.

**C** or **Save changes to USB** writes all nine settings, closes/reopens the
file and verifies every byte. B returns without writing; unsaved display
previews remain active for this boot, and edited values remain on the settings
screen. Only a successful save persists after reboot. The unsaved marker
identifies changes that have not been saved. **X** or **Discard unsaved
changes** restores the last successfully saved values, including the display
preview.

The margin moves the menu into an inset area while keeping text at native pixel
size. Text uses Ark Pixel, a bitmap font drawn pixel by pixel at 10 and 12 pixels,
so letters stay crisp on the console. The game list uses the 10-pixel design in
eleven rows, or the 12-pixel design in eight rows with large titles enabled.
Restore display defaults resets only the margin and larger-title settings.

The save writes a canonical INI containing all nine settings and replaces
comments/formatting. It requires a successfully
loaded config. Startup bypass, an invalid config, unsupported identity or a
failed transport prevents saving. The firmware cannot truncate files, so the
menu writes a fixed 512-byte plain-text file with Windows line endings, an
explicit `; End of Oh My 3DO settings.` comment, and space padding. It contains
no NUL bytes. Edit the values above the end comment; text below it is ignored.
A NUL terminates the loaded text. Do not interpret a failed save as persistent.

You can also edit `ohmy3do.ini` on a PC:

```ini
[compatibility]
folder_workaround=0

[artwork]
usb_covers=1
video_previews=0

[display]
screen_margin=0
large_titles=0

[library]
remember_position=1
recent_games=1
hide_adult=0

[audio]
preview_sound=1
```

`folder_workaround` handles FCare's directory-local ID collision bug, where the
firmware ignores entering a folder whose ID matches the one it last entered.
`0` (the default) is automatic: the workaround is used with the FCare ODE and
costs nothing unless a collision would happen. `1` uses it with any ODE and `2`
turns it off. Diagnostics shows whether it is in use. `usb_covers` and `video_previews` are 0/1; previews
default off. Margin is 0–12; large titles is 0/1. The three library settings are 0/1. `hide_adult` is a legacy fallback used only without a category configuration.
When `.ohmy3do/categories.json` exists, its per-category visibility takes precedence.
The desktop carries the old Adult visibility choice into a newly created category file. Both files retain hidden games,
including when visible favorites are removed or reordered. Missing optional settings use their defaults. Unknown/duplicate
settings or invalid values reject the config. Keep PC-edited text below 512
bytes. A full 512-byte buffer is accepted only with the generated end comment
and all eight non-`hide_adult` keys before it; `hide_adult` is optional.
CRLF, LF, UTF-8 BOM and `#`/`;` comments are accepted.

The workaround first selects a safe existing file with the directory command to
change the remembered ID without mounting it or changing the playlist. It can
instead use a safe sibling detour. Intermediate folders are not displayed. With
no safe alternative it reports an error; the separate firmware patch is the
complete fix. See [firmware/README.md](../firmware/README.md).

## Game information

The console's selected-game panel shows genre, publisher, and release date from
`.ohmy3do/metadata.3di`. Imports and **Rebuild search indexes** export this file from
the library database; the CLI `index-media.py` does the same. USB preparation
includes it automatically. The menu reads the index at the USB root and keeps
it in RAM, so browsing adds no metadata I/O. A missing genre shows **Unknown genre**;
missing publisher and date fields are left blank. When only a release year is
available, the menu shows the year. Dates come
from the game metadata and may not describe every regional pressing.
Without a metadata entry, the panel shows **No game information**.

[Project overview](../README.md)
