#!/usr/bin/env python3
"""Prepare a local, fail-closed host for this revision's bundled Anyka SDK.

No camera access or target execution occurs. The proprietary executable must be
provided by its owner and is never part of the public source archive. Only the
first16 bytes of its exported main are replaced with a direct exit125 syscall.
An approved preload constructor may use the exact bundled SDK and must _exit;
if the constructor is absent or returns, the original Tuya main cannot run.
This is transitional research support, not a clean media backend or firmware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
STUB = struct.pack('<4I', 0xe3a0007d, 0xe3a07001, 0xef000000, 0xeafffffe)
MAX_SOURCE_BYTES = 8 * 1024 * 1024


def checked_range(data, offset, size, what):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError('Out-of-bounds ' + what)
    return data[offset:offset + size]


def unpack(data, fmt, offset, what):
    return struct.unpack(fmt, checked_range(data, offset, struct.calcsize(fmt), what))


def main_location(data, expected_symbol='main'):
    """Resolve one exported ARM main using both sections and executable PT_LOAD."""
    header = checked_range(data, 0, 52, 'ELF header')
    if header[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little-endian version1')
    (kind, machine, version, entry, phoff, shoff, flags, ehsize, phentsize,
     phnum, shentsize, shnum, shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', header, 16)
    if kind != 2 or machine != 40 or version != 1:
        raise ValueError('Expected an ARM ET_EXEC executable')
    if flags & 0xff000000 != 0x05000000 or flags & 0x400:
        raise ValueError('Expected ARM EABI5 soft-float layout')
    if ehsize != 52 or phentsize != 32 or shentsize != 40 or not phnum or not shnum:
        raise ValueError('Unexpected ELF table layout')
    if shstrndx >= shnum:
        raise ValueError('Invalid ELF section-name index')
    checked_range(data, phoff, phnum * phentsize, 'program table')
    checked_range(data, shoff, shnum * shentsize, 'section table')
    sections = [unpack(data, '<10I', shoff + i * shentsize, 'section') for i in range(shnum)]
    found = []
    for section in sections:
        _, section_type, _, _, offset, size, link, _, _, entsize = section
        if section_type != 11:  # SHT_DYNSYM
            continue
        if entsize != 16 or size % entsize or link >= shnum:
            raise ValueError('Invalid dynamic symbol table')
        strings_section = sections[link]
        if strings_section[1] != 3:
            raise ValueError('Dynamic symbols do not link to a string table')
        strings = checked_range(data, strings_section[4], strings_section[5], 'dynamic strings')
        checked_range(data, offset, size, 'dynamic symbols')
        for pos in range(offset, offset + size, entsize):
            name, value, symbol_size, info, _, index = unpack(data, '<IIIBBH', pos, 'symbol')
            if name >= len(strings):
                raise ValueError('Invalid dynamic symbol name')
            terminator = strings.find(b'\0', name)
            if terminator < 0:
                raise ValueError('Unterminated dynamic symbol name')
            if strings[name:terminator] != expected_symbol.encode('ascii'):
                continue
            if info & 15 != 2 or info >> 4 not in (1, 2) or not 0 < index < shnum:
                raise ValueError('Main is not a defined exported function')
            if symbol_size < len(STUB) or value & 3:
                raise ValueError('Main is too short or not ARM-word aligned')
            own_section = sections[index]
            if not own_section[2] & 4 or own_section[1] == 8:
                raise ValueError('Main is not in a file-backed executable section')
            if value < own_section[3] or value - own_section[3] > own_section[5] - len(STUB):
                raise ValueError('Main is outside its executable section')
            section_offset = own_section[4] + value - own_section[3]
            mapped = []
            for i in range(phnum):
                p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = unpack(
                    data, '<8I', phoff + i * phentsize, 'program header')
                if p_type != 1:
                    continue
                checked_range(data, p_offset, p_filesz, 'load segment')
                if p_filesz > p_memsz:
                    raise ValueError('Load segment filesz exceeds memsz')
                if p_flags & 1 and p_vaddr <= value and value + len(STUB) <= p_vaddr + p_filesz:
                    mapped.append(p_offset + value - p_vaddr)
            if len(mapped) != 1 or mapped[0] != section_offset:
                raise ValueError('Main lacks one matching executable load mapping')
            checked_range(data, section_offset, len(STUB), 'main patch span')
            found.append((value, section_offset))
    if len(found) != 1:
        raise ValueError('Expected exactly one exported main symbol')
    return entry, *found[0]


def prepare(data, policy):
    if policy.get('schema') != 1 or policy.get('fallback_exit') != 125:
        raise ValueError('Unsupported SDK-host policy')
    if len(data) != policy['source_bytes'] or len(data) > MAX_SOURCE_BYTES:
        raise ValueError('Source size does not match the reviewed revision')
    digest = hashlib.sha256(data).hexdigest()
    if digest != policy['source_sha256']:
        raise ValueError('Source SHA256 does not match the reviewed revision')
    entry, address, offset = main_location(data, policy['main_symbol'])
    if (entry, address, offset) != (policy['elf_entry'], policy['main_address'], policy['main_file_offset']):
        raise ValueError('ELF main mapping differs from the reviewed revision')
    original = bytes.fromhex(policy['main_original_hex'])
    if len(original) != len(STUB) or data[offset:offset + len(STUB)] != original:
        raise ValueError('Original main instructions differ from the reviewed revision')
    result = data[:offset] + STUB + data[offset + len(STUB):]
    result_digest = hashlib.sha256(result).hexdigest()
    if result_digest != policy['sdk_host_sha256']:
        raise ValueError('Prepared host SHA256 differs from the reviewed result')
    report = {'schema': 1, 'board': policy['board'], 'source_sha256': digest,
              'sdk_host_sha256': result_digest, 'bytes': len(result),
              'main_address': address, 'main_file_offset': offset,
              'original_main_hex': original.hex(), 'fallback_stub_hex': STUB.hex(),
              'patch_span_bytes': len(STUB),
              'changed_byte_count': sum(a != b for a, b in zip(original, STUB)),
              'fallback_exit': 125, 'proprietary_code_retained': True,
              'clean_media_backend': False, 'flash_image': False,
              'target_executed_by_preparer': False}
    return result, report


def allowed_output(path, root=ROOT):
    path = path.resolve()
    return any(path.is_relative_to((root / directory).resolve())
               for directory in ('base/sdk', 'output'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path, help='owner-provided original anyka_ipc')
    p.add_argument('--output-dir', type=Path, default=ROOT / 'base/sdk')
    a = p.parse_args()
    try:
        if not allowed_output(a.output_dir):
            raise ValueError('Output must be under this project base/sdk/ or output/')
        source_stat = a.source.stat()
        if not stat.S_ISREG(source_stat.st_mode):
            raise ValueError('Source must be a local regular file')
        if source_stat.st_size > MAX_SOURCE_BYTES:
            raise ValueError('Source exceeds the8MiB input limit')
        policy = json.loads((ROOT / 'config/sdk-host.json').read_text())
        with a.source.open('rb') as stream:
            source = stream.read(MAX_SOURCE_BYTES + 1)
        data, report = prepare(source, policy)
        out = a.output_dir.resolve()
        host = out / 'sdk-host'
        manifest = out / 'sdk-host.json'
        if host.exists() or host.is_symlink() or manifest.exists() or manifest.is_symlink():
            raise ValueError('Prepared host or manifest already exists; refusing to replace it')
        out.mkdir(parents=True, exist_ok=True)
        # Exclusive creation never replaces an existing executable or link.
        fd = os.open(host, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o700)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        with manifest.open('x') as stream:
            stream.write(json.dumps(report, indent=2) + '\n')
        print('Prepared local SDK host:', host)
        print('Original main replaced by exit125; no camera access or execution occurred.')
        print('Proprietary code remains in this transitional host. It is not a firmware image.')
        return 0
    except (OSError, ValueError, KeyError, struct.error) as error:
        print('SDK host not prepared: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
