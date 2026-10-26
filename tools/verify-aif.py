#!/usr/bin/env python3
"""Verify a privileged 3DO AIF using only the retail public key.

Portfolio authenticates AIF data with the SignatureLen header field zeroed.
The generic `3dt sign-file --verify` command instead hashes raw file bytes.
See Portfolio's AIF format and 3dt's AIFSignatureInspector for the distinction.

The check: the file is a 3DO binary marked privileged, ends with a 64-byte
RSA signature, and that signature, opened with the retail public key, is
the PKCS #1 v1.5 padding of the MD5 of everything before it (with the
SignatureLen field zeroed).
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
RETAIL_3DO_EXPONENT = 65537

# Where the AIF header keeps its fields.
BINARY_FLAGS_OFFSET = 0x2C  # bit 30 set: a 3DO binary header follows
PRIVILEGE_OFFSET = 0xA4  # 2: a privileged executable
SIGNATURE_FIELDS_OFFSET = 0xB0  # signature offset, then signature length
SIGNATURE_LENGTH_OFFSET = 0xB4

SIGNATURE_BYTES = 64
MINIMUM_FILE_BYTES = 320

# The DER prefix that marks an MD5 digest in a PKCS #1 v1.5 signature.
MD5_DIGEST_INFO = bytes.fromhex("3020300c06082a864886f70d020505000410")


def verify(path):
    """Raises ValueError unless `path` is a privileged AIF with a valid retail signature."""
    data = bytearray(Path(path).read_bytes())
    if len(data) < MINIMUM_FILE_BYTES:
        raise ValueError("file is too short for a signed 3DO AIF")
    if not (struct.unpack_from(">I", data, BINARY_FLAGS_OFFSET)[0] & 0x40000000):
        raise ValueError("missing 3DO binary header")
    if data[PRIVILEGE_OFFSET] != 2:
        raise ValueError("expected privileged executable (flags=2)")

    offset, length = struct.unpack_from(">II", data, SIGNATURE_FIELDS_OFFSET)
    if length != SIGNATURE_BYTES or offset < 256 or offset + length != len(data):
        raise ValueError("invalid AIF signature bounds")
    signature = int.from_bytes(data[offset:], "big")
    if signature >= RETAIL_3DO_MODULUS:
        raise ValueError("signature exceeds public key modulus")

    # The signed data has the SignatureLen field zeroed.
    data[SIGNATURE_LENGTH_OFFSET : SIGNATURE_LENGTH_OFFSET + 4] = bytes(4)
    digest_info = MD5_DIGEST_INFO + hashlib.md5(data[:offset]).digest()
    padding = b"\xff" * (SIGNATURE_BYTES - len(digest_info) - 3)
    expected = b"\x00\x01" + padding + b"\x00" + digest_info
    opened = pow(signature, RETAIL_3DO_EXPONENT, RETAIL_3DO_MODULUS)
    if opened.to_bytes(SIGNATURE_BYTES, "big") != expected:
        raise ValueError("retail 3DO AIF signature mismatch")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: verify-aif.py LaunchMe")
    try:
        verify(sys.argv[1])
    except (OSError, ValueError) as exc:
        sys.exit(f"AIF verification failed: {exc}")
    print(f"{sys.argv[1]}: privileged retail 3DO signature valid")
