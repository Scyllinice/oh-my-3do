#!/usr/bin/env python3
"""Let the menu disc boot from an ODE on any XBUS unit, internal or external.

Usage: patch-boot-code.py BOOT_CODE AUDIT_JSON

The SDK's CD-DIPIR (System/Kernel/boot_code) ejects a boot disc unless it is
in the boot unit, unit 0: Portfolio's dipiru() in src/dipir/cdipir.c, which
returns DIPIR_RETURN_EXT_EJECT (6) when lde.device != BootDeviceUnit(). An
internal drive emulator is unit 0, but an ODE on the rear expansion port sits
behind the internal drive (unit 1), so the disc ejected itself there.

This removes only that check's eject, in this CD-DIPIR version (5996 bytes; see
https://3dodev.com/documentation/dipir_patching):
  0xcd4 teq r0,r1 (kept) ; 0xcd8 movne r0,#6 ; 0xcdc bne ...  ->  mov r0,r0 x2
The teq stays: with it replaced too, as 3dodev lists, the disc is still ejected
at unit 1 under the FZ-10, FZ-10 Anvil, GoldStar and Sanyo BIOSes in emulator
tests (only the FZ-1 accepts it). The SDK file is also padded to 8192 bytes;
retail discs carry this version as 5996 bytes (code to 0x16ec, then the two
64-byte signatures), and the padded file stalls those BIOSes even at unit 0.
So the patched code is trimmed to 5996 bytes. With both, the menu boots at
unit 0 and unit 1 under all five BIOSes, as the XPort's own menu disc does.
It runs after prepare-runtime.py has checked the staged file is the SDK's,
and refuses any other boot_code. The SDK copy carries signatures that no
longer match once patched, so its two signature slots are cleared; 3dt pack
then signs the patched code. The change is recorded in AUDIT_JSON.

It needs 3dt (the SDK container provides it) for the boot_code cipher.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

SDK_SHA256 = "65943649afeef92c210e61e75f0a28db1984f19b8ac68ea9b9564060d2dab3c9"
PATCHED_SHA256 = "4eb9f7f2a10777bd98637510eec64919c729e4d3a4c07bafa5cb3e99cc8ed43d"
TEQ = (0xCD4, 0xE1300001)  # teq r0,r1: checked, left in place
UNIT_CHECK = {0xCD8: 0x13A00006, 0xCDC: 0x1A00014E}  # movne r0,#6 ; bne -> no-ops
NOP = 0xE1A00000  # mov r0,r0
SIGNATURE_SLOTS = 128  # inner and outer 64-byte signatures, re-made by 3dt pack
CODE_BYTES = 0x16EC  # this CD-DIPIR's code; with its signatures, the 5996-byte retail size


def run_3dt(command, path):
    subprocess.run(["3dt", command, str(path)], check=True, stdout=subprocess.DEVNULL)


def patch(boot_code, audit):
    original = boot_code.read_bytes()
    if hashlib.sha256(original).hexdigest() != SDK_SHA256:
        raise SystemExit(f"{boot_code} is not the SDK boot_code this patch was made for")
    with tempfile.TemporaryDirectory() as work:
        code = Path(work) / "boot_code"
        shutil.copyfile(boot_code, code)
        run_3dt("decrypt-file", code)
        data = bytearray(code.read_bytes())
        if struct.unpack_from(">I", data, TEQ[0])[0] != TEQ[1]:
            raise SystemExit(f"Unexpected instruction at {TEQ[0]:#x}")
        for offset, expected in UNIT_CHECK.items():
            found = struct.unpack_from(">I", data, offset)[0]
            if found != expected:
                raise SystemExit(f"Unexpected instruction {found:#010x} at {offset:#x}")
            struct.pack_into(">I", data, offset, NOP)
        code.write_bytes(data)
        run_3dt("encrypt-file", code)
        data = bytearray(code.read_bytes())
    data = data[:CODE_BYTES] + bytes(SIGNATURE_SLOTS)
    if hashlib.sha256(data).hexdigest() != PATCHED_SHA256:
        raise SystemExit("Patched boot_code does not match the tested result")
    boot_code.write_bytes(data)

    record = json.loads(audit.read_text())
    record.setdefault("modifications", []).append({
        "path": "System/Kernel/boot_code",
        "change": "CD-DIPIR boot-unit check removed so an ODE on any XBUS unit can boot "
                  "(movne/bne at 0xcd8-0xcdc replaced with no-ops; teq at 0xcd4 kept); trimmed to "
                  "the 5996-byte retail size; signature slots "
                  "cleared for 3dt pack to sign",
        "reference": "https://3dodev.com/documentation/dipir_patching",
        "sdk_sha256": SDK_SHA256,
        "patched_sha256": PATCHED_SHA256,
    })
    audit.write_text(json.dumps(record, indent=2) + "\n")
    print("Patched boot_code: boots from an ODE on any XBUS unit")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    patch(Path(sys.argv[1]), Path(sys.argv[2]))
