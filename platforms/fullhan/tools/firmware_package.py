#!/usr/bin/env python3
"""Build/validate FH8626V100 local-daemon packages; never contact a camera.

This format is for the independently inspected /usr/bin/daemon. It is not a
Tuya-cloud OTA artifact. It does not supply download authentication or rollback.
Only kernel/rootfs records are supported, preserving bootloader and config.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

PLATFORM = b"Fuhan8626V100"
MAGIC = 0xABCD1234
BUDGET = {2: 0x1D0000, 3: 0x550000}
NAMES = {2: "kernel", 3: "rootfs"}


def check_image(kind, data):
    if kind not in BUDGET:
        raise ValueError("Only kernel (2) and rootfs (3) updates are permitted")
    if not data or len(data) > BUDGET[kind]:
        raise ValueError(f"{NAMES[kind]} exceeds its partition budget or is empty")
    if kind == 2:
        if len(data) < 64 or data[:4] != b"\x27\x05\x19\x56":
            raise ValueError("Kernel must have a legacy uImage header")
        header = bytearray(data[:64])
        expected_header = struct.unpack_from(">I", header, 4)[0]
        header[4:8] = b"\0" * 4
        if zlib.crc32(header) != expected_header:
            raise ValueError("Kernel header CRC is invalid")
        size = struct.unpack_from(">I", data, 12)[0]
        expected_data = struct.unpack_from(">I", data, 24)[0]
        if size > len(data) - 64 or zlib.crc32(data[64:64 + size]) != expected_data:
            raise ValueError("Kernel payload is truncated or its CRC is invalid")
        if data[28:31] != bytes((5, 2, 2)):
            raise ValueError("Expected Linux/ARM/kernel uImage")
        return data[:64 + size]
    if len(data) < 96 or data[:4] != b"hsqs":
        raise ValueError("Rootfs must be little-endian SquashFS")
    if struct.unpack_from("<HH", data, 28) != (4, 0):
        raise ValueError("Expected SquashFS 4.0")
    used = struct.unpack_from("<Q", data, 40)[0]
    if used < 96 or used > len(data):
        raise ValueError("SquashFS bytes_used is invalid or image is truncated")
    return data[:used]


def build(images):
    if not images:
        raise ValueError("At least one replacement image is required")
    result = bytearray(PLATFORM.ljust(20, b"\0") + struct.pack(">I", MAGIC) + bytes(8))
    for kind, data in sorted(images.items()):
        data = check_image(kind, data)
        result.extend(struct.pack(">III", kind, len(data), 0))
        result.extend(data)
    return bytes(result)


def inspect(data):
    if len(data) < 32 or data[:20].split(b"\0", 1)[0] != PLATFORM:
        raise ValueError("Wrong platform/package format; raw flash dumps are not OTA packages")
    if struct.unpack_from(">I", data, 20)[0] != MAGIC:
        raise ValueError("Invalid vendor package magic")
    offset = 32
    records = []
    seen = set()
    while offset < len(data):
        if len(data) - offset < 12:
            raise ValueError("Truncated record header")
        kind, size, address = struct.unpack_from(">III", data, offset)
        offset += 12
        if kind in seen:
            raise ValueError("Duplicate image record")
        if address != 0:
            raise ValueError("Nonzero start address is unsupported")
        if size > len(data) - offset:
            raise ValueError("Truncated record payload")
        image = data[offset:offset + size]
        check_image(kind, image)
        records.append({"type": kind, "name": NAMES[kind], "size": size,
                        "sha256": hashlib.sha256(image).hexdigest()})
        seen.add(kind)
        offset += size
    if not records:
        raise ValueError("Package contains no records")
    return {"platform": PLATFORM.decode(), "bytes": len(data), "records": records,
            "sha256": hashlib.sha256(data).hexdigest(),
            "bootloader_and_config": "not included"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("build")
    create.add_argument("--kernel", type=Path)
    create.add_argument("--rootfs", type=Path)
    create.add_argument("--output", required=True, type=Path)
    check = commands.add_parser("inspect")
    check.add_argument("package", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build":
            images = {kind: path.read_bytes() for kind, path in
                      ((2, args.kernel), (3, args.rootfs)) if path is not None}
            data = build(images)
            details = inspect(data)
            with args.output.open("xb") as output:
                output.write(data)
        else:
            details = inspect(args.package.read_bytes())
        print(json.dumps(details, indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
