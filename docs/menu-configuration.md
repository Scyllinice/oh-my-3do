# Manual menu configuration

The easiest way to change settings is **Tools → Settings** on the console or
the console settings editor in the [desktop app](../desktop/README.md).
This reference is for editing files on a PC.

## Settings file

Edit `.ohmy3do/ohmy3do.ini` on the console drive. A standard file contains:

```ini
[compatibility]
folder_workaround=0

[artwork]
usb_covers=1
video_previews=1

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

| Setting | Values |
| --- | --- |
| `folder_workaround` | `0`: Auto; `1`: On; `2`: Off. Auto is recommended for the FCare ODE. |
| `screen_margin` | 0–12 pixels |
| Other settings | `0`: Off; `1`: On |

`usb_covers` is the **Show covers** setting. `hide_adult` applies only when
there is no category configuration; otherwise `.ohmy3do/categories.json`
controls visibility. Manage categories through the desktop app or console Settings.

Keep manual files below 512 bytes. Unknown keys, duplicate keys and invalid
values cause the configuration to be rejected. Missing optional settings use
their defaults. There is no theme or background-music setting.

When editing a file saved by the console, keep its end comment and edit only
the settings above it. Do not remove the trailing padding. Restart the console
after editing on a PC.

The folder workaround addresses an ODE navigation bug. Leave navigation-helper
files in place; **Drive setup** repairs them. The [firmware reference](../firmware/README.md)
explains the bug and workaround.

## Favorites file

Favorites can be managed on the console. To edit them manually, use
`.ohmy3do/favorites.txt`, with one game path per line, relative to the drive root:

```text
Games/Road Rash.iso
Games/D/D (Disc 1).iso
```

Use forward slashes, include extensions, and omit drive letters. Up to 64 entries
are supported. Blank lines and lines starting with `#` or `;` are accepted.
The console rewrites formatting and comments when favorites are edited there.

[Menu guide](menu-guide.md)
