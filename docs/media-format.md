# USB menu media formats

All integers below are unsigned big-endian. The menu reads these files using
LOCODE E1/E3/E7/E5, with E6 buffer setup. It does not seek or write media files.
Firmware may create an empty missing file even when opening it for a read.

## Association index

`covers/index.3di` and `previews/index.3di` share this layout:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DIDX1` |
| 8 | 4 | Entry count, at most 2048 |
| 12 | 4 | Payload bytes, at most 131072 |
| 16 | 4 | Standard CRC32 of payload |
| 20 | 4 | Reserved, zero |

Each payload row is two 16-bit lengths followed by a NUL-terminated USB-relative
game path and media basename. Lengths include terminators. Rows sort by ASCII
case-insensitive path, with no duplicate paths. A media basename is at most 96
UTF-8 bytes, has no path separators/control characters, and ends in `.png`,
`.3dc` or `.3dv`. Covers accept PNG or legacy 3DC; previews accept 3DV.
`tools/index-media.py` builds associations without converting media.

## Silent preview

`previews/Title.3dv`:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DVID1` |
| 8 | 2 | Width, 64 |
| 10 | 2 | Height, 80 |
| 12 | 4 | Frame payload bytes |
| 16 | 4 | Standard CRC32 of all frame bytes |
| 20 | 2 | Frame count, 1–32 |
| 22 | 2 | Frames per second, 1–12 |
| 24 | variable | Consecutive frames, each 10240 bytes |
| after frames | 8 | `END3DV01` |

Each frame is row-major packed RGB555: `0rrrrrgggggbbbbb`, with the high byte
first. Frame data is cached in RAM and copied to an uncoded 16-bit CEL at the
configured rate. There is no audio. The final marker is checked with a poisoned
read buffer because firmware reports requested read lengths even at EOF.

## PNG cover

PNGs are decoded directly, with chunk CRC, zlib completion, exact decoded-length
and IEND checks. The IEND CRC is verified with a poisoned read buffer to reject
stale EOF data. Limits are 1024×1024 pixels and 524288 bytes. The streaming
decoder retains a 32 KiB inflate dictionary, decoder state, a scanline buffer and
small scaling tables, rather than an entire source-size bitmap. Nearest-neighbor
sampling fits the image into 64×80, with letterboxing and alpha compositing over
RGB `(8,16,33)`. Optional gamma and background-color metadata do not change this.
