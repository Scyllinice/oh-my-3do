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
- The logo was generated for this project. The startup/loading screens compose
  that logo with shapes, text and static. The animated gecko uses a generated
  sprite sheet. Sources, prompts and preparation are documented in
  `assets/README.md`.
- Demo cover imagery derives from the project's loading-screen artwork. The
  repository contains no game disc images. Commercial artwork and footage
  appear only in documentation screenshots.
  Documentation screenshots show Road Rash gameplay and cover artwork from the
  user's local library, captured through the emulator. Those depictions retain
  their respective rights; they are not bundled as playable previews or cover
  assets in the menu, desktop app or media-tools packages.
- Personal cover associations, converted covers, USB backups, ISO builds and
  downloaded dependencies are excluded by `.gitignore`.
- The PC library organizer downloads identification data from
  [Redump](https://redump.info/datfile/3do/), metadata from
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

The menu's font is Ark Pixel, a bitmap font by TakWolf drawn at 10 and 12 px
(https://github.com/TakWolf/ark-pixel-font). Its proportional Latin glyphs are
converted into `src/ui_font_data.h`, so rebuilding the menu needs no font
installation; `tools/build-ui-font.py` regenerates them with Pillow from the
release TTFs. The bold button letters are computed from the same glyphs. The
font license follows and is also in `vendor/fonts/ark-pixel-OFL.txt`.

```text
Ark Pixel Font
https://github.com/TakWolf/ark-pixel-font

Copyright (c) 2021, TakWolf (https://takwolf.com).

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://openfontlicense.org


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
