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

Signed ISO outputs contain SDK runtime components. Consult the dependency terms
when redistributing compiled images; this source license does not grant rights
to dependency components.

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
