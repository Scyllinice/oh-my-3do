# PNG decoder provenance

Pngle and its bundled miniz v1.15 come from
https://github.com/kikuchan/pngle at commit
`b1c68193f1d3f8642b3e0e095d457a828038e6fb` (`src/`). Pngle is MIT licensed;
miniz retains its upstream public-domain notice. Original notices remain in the
source files. `pngle_types.h` is a project portability header.

Local changes make the decoder build with the legacy C89 Norcroft compiler:
C89 block/loop declarations and comments, no inline/variadic debug macros,
32-bit integer types, and no unused 64-bit/archive/compression support. The
inflate backend uses byte-aligned accesses, explicit endian detection and a
32-bit bit buffer. Gamma correction is disabled; no floating-point math is used.

Additional changes check the complete inflated length before IEND, handle empty
Adam7 passes without unsigned underflow, offer cancellation from a draw callback,
ignore optional bKGD metadata in favor of the menu background, avoid signed-shift
undefined behavior and speed up 8-bit color conversion. The caller bounds image
dimensions/file bytes and verifies the final IEND CRC using a poisoned USB buffer.
