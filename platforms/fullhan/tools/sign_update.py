#!/usr/bin/env python3
"""Sign a validated local-daemon package with an existing Ed25519 key.

No key generation and no camera interaction. Private keys stay on the operator
workstation; verification keys are raw 32-byte Ed25519 public keys.
"""
import argparse
import hashlib
import struct
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from firmware_package import inspect  # noqa: E402

MAGIC = b"FHCAMO1\0"
BOARD = b"fh8626v100-mis2006-8m-v1".ljust(32, b"\0")
HEADER_SIZE = 192
MAX_PAYLOAD = 32 + 24 + 0x1D0000 + 0x550000


def check_payload(payload):
    """Match the target verifier's canonical image policy before signing."""
    inspect(payload)
    if payload[:20] != b"Fuhan8626V100".ljust(20, b"\0") or payload[24:32] != bytes(8):
        raise ValueError("vendor header must be canonical")
    position = 32
    previous = 0
    while position < len(payload):
        kind, size, address = struct.unpack_from(">III", payload, position)
        position += 12
        image = payload[position:position + size]
        if kind <= previous or address:
            raise ValueError("records must be unique, ordered and have zero start address")
        if kind == 2:
            if (size != 64 + struct.unpack_from(">I", image, 12)[0] or image[31] != 0 or
                struct.unpack_from(">II", image, 16) != (0xA0008000, 0xA0008000)):
                raise ValueError("kernel size/load/entry mismatch")
        elif kind == 3:
            if (struct.unpack_from("<H", image, 20)[0] != 4 or
                struct.unpack_from("<Q", image, 40)[0] != size):
                raise ValueError("rootfs must be canonical XZ SquashFS")
        previous = kind
        position += size


def envelope(payload, key, sequence, board=BOARD):
    if not 0 < sequence < 2**64:
        raise ValueError("sequence must be in 1..2^64-1")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("package exceeds total partition limits")
    check_payload(payload)
    header = (MAGIC + struct.pack("<II", 1, HEADER_SIZE) + board +
              struct.pack("<QQ", sequence, len(payload)) + hashlib.blake2b(payload).digest())
    if len(board) != 32 or len(header) != 128:
        raise ValueError("hardware identity must occupy exactly 32 bytes")
    return header + key.sign(header) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-key", required=True, type=Path,
                        help="existing unencrypted Ed25519 PEM key; never sent to camera")
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--public-key-output", type=Path,
                        help="optional new file containing the corresponding raw public key")
    args = parser.parse_args()
    try:
        key = serialization.load_pem_private_key(args.private_key.read_bytes(), password=None)
        if not isinstance(key, Ed25519PrivateKey):
            raise ValueError("an Ed25519 key is required")
        if args.input.stat().st_size > MAX_PAYLOAD:
            raise ValueError("package exceeds total partition limits")
        payload = args.input.read_bytes()
        data = envelope(payload, key, args.sequence)
        with args.output.open("xb") as output:
            output.write(data)
        if args.public_key_output:
            public = key.public_key().public_bytes(serialization.Encoding.Raw,
                                                   serialization.PublicFormat.Raw)
            with args.public_key_output.open("xb") as output:
                output.write(public)
        print(f"Signed {len(payload)} payload bytes, sequence={args.sequence}")
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
