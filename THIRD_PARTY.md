# Dependencies and asset provenance

The root MIT license covers this project's original source and documentation.
Dependencies retain their own licenses and notices; the MIT license does not
relicense the 3DO SDK, Portfolio OS, firmware, games, or third-party artwork.

- [3do-devkit](https://github.com/trapexit/3do-devkit) supplies the legacy compiler,
  SDK headers/libraries, disc filesystem and tools inside the Docker image.
  Consult that repository and the included components for their terms.
- [Portfolio OS](https://github.com/trapexit/portfolio_os) and
  [FCare/SataTo3DO](https://github.com/FCare/SataTo3DO) are reference checkouts used
  by optional tests. They are fetched separately into ignored `third_party/`;
  this repository does not vendor their source or toolchain binaries.
- The firmware patch targets FCare's code and retains its upstream licensing.
- The logo and loading-screen images were generated for this project; their
  prompts and preparation are documented in `assets/README.md`.
- Demo cover imagery derives from the project's loading-screen artwork. The
  repository contains no commercial game images or game box scans.
- Personal cover associations, converted covers, USB backups, ISO builds and
  downloaded dependencies are excluded by `.gitignore`.
- The PC library organizer downloads identification data from
  [Libretro/Redump](https://github.com/libretro/libretro-database), metadata from
  [LaunchBox](https://forums.launchbox-app.com/topic/30123-launchbox-games-db-external-scraper/),
  and covers from [Libretro thumbnails](https://github.com/libretro-thumbnails/libretro-thumbnails).
  These datasets and scans are fetched into a local cache, are not bundled with
  the source, media tools ZIP or ISO, and retain their respective terms and
  artwork rights. Generated databases record source URLs and data checksums;
  this project's MIT license covers the organizer code, not downloaded data.

Signed ISO outputs contain SDK runtime components. Consult the dependency terms
when redistributing compiled images; this source license does not grant rights
to dependency components.

The PC scanner's Mode 1 sector encoder is original project code implementing
[ECMA-130](https://ecma-international.org/publications-and-standards/standards/ecma-130/).
It is covered by the root MIT license. Native PC accelerators are built from
`tools/sector-codec.c`; they contain no third-party encoder or CRT dependencies
in the Windows build. Synthetic test vectors were checked with an external
UNECM executable; its GPL source and executable are not bundled or linked.

The ISO retains the complete pinned SDK bootstrap runtime. The build records
runtime components, linked libraries and their hashes in `runtime-audit.json`.

The vendored [Pngle decoder](https://github.com/kikuchan/pngle) is MIT licensed.
Its bundled miniz v1.15 retains its public-domain notice. Their source notices
are preserved in `vendor/pngle/`; the pinned revision and local portability and
validation changes are listed in [vendor/pngle/README.md](vendor/pngle/README.md).
Signed menu binaries include the PNG decoder and inflate code. The video preview
format/player and PC conversion/index tools are original project code; FFmpeg is
used as an external conversion tool in Docker, with its installed package notices.

## Pngle notice

```text
MIT License

Copyright (c) 2019 kikuchan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## miniz notice

```text
This is free and unencumbered software released into the public domain.

  Anyone is free to copy, modify, publish, use, compile, sell, or
  distribute this software, either in source code form or as a compiled
  binary, for any purpose, commercial or non-commercial, and by any
  means.

  In jurisdictions that recognize copyright laws, the author or authors
  of this software dedicate any and all copyright interest in the
  software to the public domain. We make this dedication for the benefit
  of the public at large and to the detriment of our heirs and
  successors. We intend this dedication to be an overt act of
  relinquishment in perpetuity of all present and future rights to this
  software under copyright law.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
  OTHER DEALINGS IN THE SOFTWARE.

  For more information, please refer to <http://unlicense.org/>
```

## OM3DO UI bitmap font

The menu's OM3DO UI font is a rasterized, renamed derivative of DejaVu Sans.
Generated glyphs are checked into `src/ui_font_data.h`; rebuilding the menu does
not require a font installation. `tools/build-ui-font.py` regenerates them with
Pillow and a local DejaVu Sans TTF. The font license follows.

```text
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: DejaVu fonts
Upstream-Author: Stepan Roh <src@users.sourceforge.net> (original author),
                  see /usr/share/doc/fonts-dejavu-core/AUTHORS for full list
Source: https://dejavu-fonts.github.io/

Files: *
Copyright: Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
 Bitstream Vera is a trademark of Bitstream, Inc.
 DejaVu changes are in public domain.
License: bitstream-vera
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of the fonts accompanying this license ("Fonts") and associated
 documentation files (the "Font Software"), to reproduce and distribute the
 Font Software, including without limitation the rights to use, copy, merge,
 publish, distribute, and/or sell copies of the Font Software, and to permit
 persons to whom the Font Software is furnished to do so, subject to the
 following conditions:
 .
 The above copyright and trademark notices and this permission notice shall
 be included in all copies of one or more of the Font Software typefaces.
 .
 The Font Software may be modified, altered, or added to, and in particular
 the designs of glyphs or characters in the Fonts may be modified and
 additional glyphs or characters may be added to the Fonts, only if the fonts
 are renamed to names not containing either the words "Bitstream" or the word
 "Vera".
 .
 This License becomes null and void to the extent applicable to Fonts or Font
 Software that has been modified and is distributed under the "Bitstream
 Vera" names.
 .
 The Font Software may be sold as part of a larger software package but no
 copy of one or more of the Font Software typefaces may be sold by itself.
 .
 THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
 TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
 FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
 ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
 FONT SOFTWARE.
 .
 Except as contained in this notice, the names of Gnome, the Gnome
 Foundation, and Bitstream Inc., shall not be used in advertising or
 otherwise to promote the sale, use or other dealings in this Font Software
 without prior written authorization from the Gnome Foundation or Bitstream
 Inc., respectively. For further information, contact: fonts at gnome dot
 org.

```

## 3at (host audio conversion only)

Music preparation uses [3at](https://github.com/trapexit/3at), pinned in the Docker
image to `4b47aadfaba31b27519d4a84d2ef9bdfeb61d229`. It encodes blockless ADP4
with its bundled adpcm-xq encoder. Its ISC license and David Bryant's BSD license
remain in `/opt/3at` and `/usr/local/share/doc/3at` in the development image.
3at is not included in the console ISO or the portable media-tools ZIP. The
portable script invokes a separately installed copy. Personal input music and
converted tracks are not included in project releases.
