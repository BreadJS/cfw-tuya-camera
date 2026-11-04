#!/usr/bin/env python3
"""Build/inspect the exact U-Boot SD linux.bin format, offline only.

Distinct from firmware_package (daemon packages) and signed web OTA capsules.
Only complete kernel/rootfs replacement is admitted; protected NOR ranges are
never records. The stock bootloader checks CRC32, not a cryptographic signature.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

import firmware_package as firmware

START = {2: 0x90000, 3: 0x260000}
BUDGET = dict(firmware.BUDGET)
ERASE_SIZE = 0x10000
FLASH_SIZE = 0x800000
PLATFORM = firmware.PLATFORM.ljust(20, b'\0')
MAGIC = firmware.MAGIC
PACKAGE_BYTES = 32 + 24 + sum(BUDGET.values())


def build(images):
    """Return pinned-layout SD bytes from {2: kernel bytes,3: rootfs bytes}.

    Source filesystem/uImage padding is canonicalized by firmware_package;
    both full NOR partitions are then filled with FF outside declared images.
    """
    if set(images) != {2, 3}:
        raise ValueError('SD deployment requires exactly kernel2 and rootfs3')
    payload = bytearray()
    for kind in (2, 3):
        image = firmware.check_image(kind, images[kind])
        payload.extend(struct.pack('>III', kind, BUDGET[kind], START[kind]))
        payload.extend(image)
        payload.extend(b'\xff' * (BUDGET[kind] - len(image)))
    data = PLATFORM + struct.pack('>III', MAGIC, zlib.crc32(payload), len(payload)) + payload
    inspect(data)
    return bytes(data)


def inspect(data):
    """Validate the complete fixed safe SD layout; return JSON-ready metadata.

    Accepts neither partial images, arbitrary flash addresses, extra records,
    nor non-FF tails. This is stricter than the bootloader itself.
    """
    if len(data) != PACKAGE_BYTES:
        raise ValueError('SD package must contain exactly two complete partitions')
    if data[:20] != PLATFORM:
        raise ValueError('Wrong canonical platform header')
    magic, crc, size = struct.unpack_from('>III', data, 20)
    if magic != MAGIC or size != len(data) - 32:
        raise ValueError('Wrong magic or total payload length')
    if zlib.crc32(data[32:]) != crc:
        raise ValueError('SD payload CRC32 mismatch')
    records = []
    offset = 32
    for expected in (2, 3):
        if len(data) - offset < 12:
            raise ValueError('Truncated SD record')
        kind, count, address = struct.unpack_from('>III', data, offset)
        offset += 12
        if kind != expected or count != BUDGET[expected] or address != START[expected]:
            raise ValueError('Protected, duplicate, reordered, overlapping or incorrect partition record')
        end = address + count
        if address % ERASE_SIZE or count % ERASE_SIZE or end > FLASH_SIZE:
            raise ValueError('Unaligned or out-of-range NOR write')
        if (kind == 2 and end != START[3]) or (kind == 3 and end != 0x7b0000):
            raise ValueError('Protected NOR boundary crossed')
        if count > len(data) - offset:
            raise ValueError('Truncated SD image')
        padded = data[offset:offset + count]
        canonical = firmware.check_image(kind, padded)
        if padded[len(canonical):] != b'\xff' * (count - len(canonical)):
            raise ValueError('Non-FF bytes outside canonical image')
        records.append({'type': kind, 'name': firmware.NAMES[kind],
                        'start_address': address, 'size': count,
                        'image_bytes': len(canonical), 'erase_blocks': count // ERASE_SIZE,
                        'image_sha256': hashlib.sha256(canonical).hexdigest(),
                        'partition_sha256': hashlib.sha256(padded).hexdigest()})
        offset += count
    if offset != len(data):
        raise ValueError('Unexpected trailing record/data')
    return {'format': 'fh8626v100-uboot-sd', 'platform': firmware.PLATFORM.decode(),
            'bytes': len(data), 'payload_bytes': size, 'payload_crc32': f'{crc:08x}',
            'records': records, 'sha256': hashlib.sha256(data).hexdigest(),
            'protected_mtd': [0, 1, 2, 5], 'signed': False,
            'bootloader_readback_verified': False,
            'sd_filename': 'linux.bin', 'sd_file_automatically_removed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--rootfs', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--inspect', type=Path)
    args = parser.parse_args()
    try:
        if args.inspect:
            if args.kernel or args.rootfs or args.output:
                raise ValueError('--inspect cannot be combined with build arguments')
            details = inspect(args.inspect.read_bytes())
        else:
            if not all((args.kernel, args.rootfs, args.output)):
                raise ValueError('--kernel, --rootfs and --output are required')
            data = build({2: args.kernel.read_bytes(), 3: args.rootfs.read_bytes()})
            details = inspect(data)
            with args.output.open('xb') as output:
                output.write(data)
        print(json.dumps(details, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f'Error: {error}\n')


if __name__ == '__main__':
    main()
