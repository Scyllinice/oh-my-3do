# Console menu guide

Browse games, manage disc queues and open tools with the controller. For drive
setup, see [installation](installation.md).

## Controls

P is the controller's Start button. Hold it for about three quarters of a
second to see help for the current screen. Press B or P to close help.

| Control | Action |
| --- | --- |
| Up / Down | Move selection; hold to repeat |
| Left / Right or L / R | Page through the list |
| A | Open a folder or play a game |
| B | Go back |
| C | Add the selected game to the disc queue |
| Tap P | Review the queue |
| Hold P | Show controls for the current screen |
| X | Switch cover/video; hold for full-screen viewing |
| L + R | Open Tools |

For L+R, hold one shoulder button and add the other. This opens Tools without
turning a page first. Tools remembers the last highlighted tool.

Useful shortcuts:

- **Hold C + tap P:** show the selected game's information and description.
- **Hold C + A:** play only the selected disc, or open a game folder.
- **Hold C + Up:** select the alphabet strip.
- **Hold C + Left / Right:** jump to the previous or next initial letter.
- **Hold X at startup:** start without optional USB settings and media, for troubleshooting.

## Browse and play

Select a game and press **A**. For BIN/CUE games, select the CUE file.
The menu hides region and language tags from displayed names but keeps edition,
revision and disc labels. Numbered markers distinguish otherwise identical names.
Files and folders are not renamed.

Folders containing one game appear as a single playable entry when the drive's
indexes are prepared. Hold **C + A** to open that folder instead.

Covers and previews appear after you pause on a selection. Tap **X** to switch
between them; hold **X** to expand the current cover or playing preview. Press
any button to leave full-screen viewing. See the [media guide](media-guide.md)
for preparing artwork.

**Hold C + tap P** opens Game details. Use Left/Right to turn pages and B to
return. Add or edit descriptions in the desktop app, then copy the updated
information to the console drive.

The menu hides its support files. Leave files named
`Oh My 3DO - Folder Navigation Helper (Keep).iso` in place: they help the ODE
navigate folders reliably. **Drive setup** in the desktop app repairs missing helpers.

## Disc queue and automatic grouping

Use the queue for discs from **the same multi-disc game**, in playing order.
It is not a way to switch between unrelated games.

With an empty queue, playing Disc 1 of a recognized set automatically queues
its remaining discs. Starting at Disc 2 queues Disc 2 onward. The discs must be
in the same folder, share a game name and be numbered consecutively from 1
using labels such as `Disc 1`, `Disk 1` or `CD1`. Queue them manually if they
are in separate folders or are not recognized. **Hold C + A** plays just one disc.

To build a queue manually, press **C** on each image, then **P** to review it.
The queue holds up to 16 discs and is cleared when the console powers off.

| Control | Queue review |
| --- | --- |
| A / P | Launch in the displayed order |
| X | Remove the selected disc |
| C | Start reordering; Up/Down moves the disc, C or B finishes |
| Hold C + X | Clear the queue |
| B | Return to browsing |

The ODE advances when the game requests the next disc. The menu does not run
in the background after a game starts and cannot provide an in-game overlay.

## Favorites and letter navigation

Select a game, then open **Tools → Favorites**. Press **C** to add it.
In Favorites, **A** locates a game, **X** removes it, and **P** reloads the list.
Press A again after locating a game to play it.

Press **R** to reorder favorites, move with Up/Down, then press **A or R** to
save. **B** cancels reordering. Favorites are saved on the drive and support
up to 64 entries.

For letter navigation, press **C + Up**, or move past either end of the game
list. Choose a letter with Left/Right and press A to jump. B cancels; `#`
covers numbers and other initials. You can also hold C and press Left/Right
to jump between initials directly.

## Remembered position, recent games and finding names

The menu normally returns to the last folder and selection when started.
Disable this with **Tools → Settings → Remember position**.

**Tools → Recent games** lists recent launch selections. A locates a game;
press A again to play. Recent-game recording can also be disabled in Settings.

**Tools → Find a game** searches game names across the selected drive. Use
the arrows and A to type, X to erase, C to clear, and P to search. Leave the
query blank to list all games. A locates a result; B returns to the keyboard.

Search needs an up-to-date index. The desktop app prepares it when copying
games. After manual drive changes, run `py index-media.py M:/` from the extracted
media tools, replacing `M:/` with your drive letter.

## Display preferences and configuration

Open **Tools → Settings** to change screen margins, title size, covers,
previews, sound, remembered position and recent games. Use Left/Right or A to
change a value, and **C** to save. Some options require a cold boot.

**B** leaves Settings without saving. **X** discards unsaved changes.
`UNSAVED` means changes have not yet been saved to the drive.

**Categories…** shows or hides categories immediately without deleting games.
Use the desktop app to add or rename categories.

Leave **Folder workaround** on **Auto** unless troubleshooting calls for a
change. The [configuration reference](menu-configuration.md) covers manual
INI editing and favorites files.

## Game information

The selected-game panel shows available genre, publisher and release date.
For descriptions and more details, **hold C and tap P**.

Use **Get game information…**, **Prepare missing media**, or **Game details →
Edit information…** in the desktop app, then copy the updated information to
the drive. Descriptions remain available offline.

For connection errors or failed media loads, see [troubleshooting](troubleshooting.md).

[Project overview](../README.md)
