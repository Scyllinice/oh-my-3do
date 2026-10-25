#!/usr/bin/env python3
"""Verify a privileged 3DO AIF using only the retail public key.

Portfolio authenticates AIF data with the SignatureLen header field zeroed.
The generic `3dt sign-file --verify` command instead hashes raw file bytes.
See Portfolio's AIF format and 3dt's AIFSignatureInspector for the distinction.
"""

import hashlib
from pathlib import Path
import struct
import sys


RETAIL_3DO_MODULUS = int(
    "B19462B00D8D6E1EC909AB385E06FE034BFD282E9FFDC584838C15F12593DD1E3"
    "A8B5626F1B9D0ED0C384EF6C5D14512BD72DDB85B44080E0472C03D0AFC4C97",
    16,
)


def verify(path):
    data = bytearray(Path(path).read_bytes())
    if len(data) < 320:
        raise ValueError("file is too short for a signed 3DO AIF")
    if not (struct.unpack_from(">I", data, 0x2C)[0] & 0x40000000):
        raise ValueError("missing 3DO binary header")
    if data[0xA4] != 2:
        raise ValueError("expected privileged executable (flags=2)")
    offset, length = struct.unpack_from(">II", data, 0xB0)
    if length != 64 or offset < 256 or offset + length != len(data):
        raise ValueError("invalid AIF signature bounds")
    signature = int.from_bytes(data[offset:], "big")
    if signature >= RETAIL_3DO_MODULUS:
        raise ValueError("signature exceeds public key modulus")
    data[0xB4:0xB8] = bytes(4)
    digest = hashlib.md5(data[:offset]).digest()
    digest_info = bytes.fromhex("3020300c06082a864886f70d020505000410") + digest
    expected = b"\x00\x01" + b"\xff" * (64 - len(digest_info) - 3) + b"\x00" + digest_info
    actual = pow(signature, 65537, RETAIL_3DO_MODULUS).to_bytes(64, "big")
    if actual != expected:
        raise ValueError("retail 3DO AIF signature mismatch")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: verify-aif.py LaunchMe")
    try:
        verify(sys.argv[1])
    except (OSError, ValueError) as exc:
        sys.exit(f"AIF verification failed: {exc}")
    print(f"{sys.argv[1]}: privileged retail 3DO signature valid")
