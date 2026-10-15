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

## USB menu music (`music/menu.3dm`)

The music file is loaded once, then played from MEMTYPE_AUDIO RAM by the native
ADP4 DSP player. It is not streamed. No index is needed for this fixed path.
All integers in the container are big-endian:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DMUS2` |
| 8 | 4 | Sample rate: 11025 or 22050 |
| 12 | 4 | Decoded mono frame count, divisible by eight, at least eight |
| 16 | 4 | Compressed byte count: frames / 2, at most 786432 |
| 20 | 4 | CRC32 of the compressed payload |
| 24 | byte count | Blockless Intel/DVI ADP4, high nibble first, initial predictor/index zero |
| after payload | 8 | `END3DM02` |

Decoded samples are signed 16-bit mono. The Sample Item uses `AF_TAG_NUMBITS=16`,
`AF_TAG_COMPRESSIONTYPE=ADP4`, and `AF_TAG_COMPRESSIONRATIO=4`; eight-frame
alignment makes the compressed DMA data quad-byte aligned. The complete sample
is the sustain loop. `decodeadpcm.dsp` is loaded before `adpcmvarmono.dsp`.
Its Frequency knob uses phase increments 0x2000 at 11025 Hz and 0x4000 at 22050 Hz.

The loader verifies the CRC and reads the known footer after poisoning all eight
expected bytes in the firmware buffer. This rejects truncated files even when
EOF returns a stale buffer and the firmware claims the full requested count.
No file writes, seeks, playlist or mount commands are used to load music.
