# USB menu media formats

All integers below are unsigned big-endian. The menu reads these files using
LOCODE E1/E3/E7/E5, with E6 buffer setup. It does not seek or write media files.
Firmware may create an empty missing file even when opening it for a read.

Support paths such as `covers/index.3di` are relative to `.ohmy3do/` in the
consolidated layout, selected by its nonempty `ohmy3do.ini`. Legacy root layouts
remain supported. Game-path keys inside indices remain USB-volume-relative.

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
`.3dc` or `.3dv`. Covers accept PNG or prepared/legacy 3DC; previews accept 3DV.
`tools/index-media.py` builds associations without converting media.

## Legacy silent preview (still supported)

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
small scaling tables, rather than an entire source-size bitmap. Area
sampling fits covers into 96×120, with letterboxing and alpha compositing over
RGB `(8,16,33)`. Optional gamma and background-color metadata do not change this.

## Prepared cover (menu v0.16.0-rc12+)

The library tools produce `.3dc` files directly in the console's RGB555 format:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DCOV2` |
| 8 | 2 | Width, 96 |
| 10 | 2 | Height, 120 |
| 12 | 4 | Compressed payload bytes, 1–32768 |
| 16 | 4 | CRC32 of compressed payload |
| 20 | 4 | Decoded bytes, 23040 |
| 24 | variable | One zlib stream, row-major big-endian RGB555 |
| after payload | 8 | `END3DC02` |

The decoder writes directly into the cover CEL. It checks the exact compressed
and decoded lengths, CRC and poisoned footer. No PNG scanline filters, scaling
or color callbacks run. The 96×120 display pixels match a prepared PNG after
RGB555 conversion. PNG and legacy `OM3DCOV1` covers remain supported.

## Compressed preview with audio (menu v0.16.0+)

All integers are big-endian. New converters produce 160×120 landscape frames,
requiring menu v0.16.0-rc10+. Only the pairs 160×120 and legacy 64×80 are
accepted; version 1 remains 64×80 only. `previews/Title.3dv` uses:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | `OM3DVID2` |
| 8 | 2 | Width, 160 (legacy: 64) |
| 10 | 2 | Height, 120 (legacy: 80) |
| 12 | 4 | Combined video/audio payload bytes, at most 1048576 |
| 16 | 4 | Standard CRC32 of video followed by audio |
| 20 | 2 | Frame count, 1–720, no more than fps × 120 |
| 22 | 2 | Frames per second, 1–15 (15 fps requires v0.16.0-rc2+) |
| 24 | 4 | Video payload bytes, including the offset table |
| 28 | 4 | Audio payload bytes, zero for silent clips |
| 32 | 4 | Audio sample rate, 11025 or zero when silent |
| 36 | 4 | Audio sample count, equal to audio bytes |
| 40 | video bytes | Frame-offset table followed by compressed frames |
| after video | audio bytes | Signed 8-bit mono PCM |
| after payload | 8 | `END3DV02` |

The video starts with frame_count + 1 unsigned 32-bit offsets, relative to the
start of the video payload. The first equals the table length; the last equals
the video byte count. Offsets strictly increase. Each independent zlib stream
must consume its exact interval and decode to exactly width × height × 2 RGB555 bytes (38400 for 160×120), with
the same pixel layout as version 1. A reusable heap-allocated inflater and one
frame buffer avoid storing expanded frames or large decoder state on the stack.
The frame buffer is allocated in CEL-accessible memory and borrowed by the
preview CEL. It is the decode destination as well as the displayed texture;
there is no second frame copy. The CEL is destroyed before its borrowed frame
buffer is freed. Optional decoded cover-cache entries are released before
loading a matched preview, leaving the currently displayed cover available.

Audio length is `align4(ceil(frame_count * 11025 / fps))`. The converter pads
short audio with silence and applies short fades at the endpoints. Audio lives
in MEMTYPE_AUDIO storage; the Sample Item uses 8 bits, one channel, 11025 Hz.
`varmono8.dsp` plays at Frequency phase increment 0x2000 and routes to both
channels of `directout.dsp`. There is no background-music sample or USB stream.
The menu restarts video and its sample together at every loop boundary, closes
audio before freeing its borrowed PCM, and releases the entire clip on input.

The loader checks lengths, CRC, the poisoned footer read, every frame boundary
and the first compressed frame before playback. Later frames are checked and
decompressed when displayed, avoiding an eager decode pass over the whole clip.
The PC validator still checks every frame. Partial reads, invalid offsets, bad zlib lengths,
cancellation and allocation failures release all owned memory. CRC covers both
buffers incrementally, avoiding a second copy of the audio in general-purpose
RAM. Original video sources and older `.3dv` files are never modified by loading.

The converter caps new clips at 15 seconds at 15 fps, shortening further to fit
the 512 KiB RAM budget without lowering frame rate. This is an authoring policy; the
player still accepts older longer clips within the format limits above.
Older version-1 previews retain their 1–12 fps limit.

Menu v0.16.0-rc12 caches compressed video in allocations of at most 4096 bytes,
with a 512 KiB combined video/table/audio ceiling and a 64 KiB free-DRAM reserve.
The inflater can read across block boundaries. Older files up to the 1 MiB
format limit still load, retaining the longest complete frame/audio prefix that
fits; the remaining source bytes are discarded while validating the full CRC.
Sound-off skips PCM allocation. Playback never streams from USB.
