#!/usr/bin/env python3
"""Offline exact-source pins and bytearray NOR model; never access a device."""
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
sys.path.insert(0, str(ROOT/'tests/support'))
import sd_package as sd
from fixtures import kernel, rootfs


def envelope(payload):
    return sd.PLATFORM + struct.pack('>III', sd.MAGIC, zlib.crc32(payload), len(payload)) + payload


def apply_fake(flash, data, stop_after=None):
    sd.inspect(data)
    offset, changed = 32, 0
    while offset < len(data):
        _, size, address = struct.unpack_from('>III', data, offset)
        offset += 12
        for count in range(0, size, sd.ERASE_SIZE):
            block = data[offset+count:offset+count+sd.ERASE_SIZE]
            start = address+count
            if flash[start:start+sd.ERASE_SIZE] != block:
                if stop_after is not None and changed == stop_after:
                    return changed
                flash[start:start+sd.ERASE_SIZE] = b'\xff'*sd.ERASE_SIZE
                flash[start:start+sd.ERASE_SIZE] = block
                changed += 1
        offset += size
    return changed


class SDTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.images = {2: kernel(), 3: rootfs()}
        cls.data = sd.build(cls.images)

    def test_exact_shipped_source_pins(self):
        source = (ROOT/'tests/fixtures/uboot.bin').read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(), '517ecc5f0e69d763eb6d13f4db3ae5284a1067cedef61ad2755d82de654c2e34')
        # ARM LE bytes at observed endian swap, CRC, payload length, compare,
        # physical record address load and reset-type condition anchors.
        for address, code in {
            0xa0807070: '203ca0e1', 0xa08071e8: '180095e5',
            0xa0807200: 'cc3c00eb', 0xa08074b8: '1c0095e5',
            0xa08073b8: '2c009de5', 0xa08072a0: 'a24800eb',
            0xa0807478: '010050e3',
        }.items():
            self.assertEqual(source[address-0xa0800000:address-0xa0800000+4].hex(), code)
        table = source[0x2521c:0x2561c]
        expected = []
        for value in range(256):
            crc = value
            for _ in range(8):crc = (crc>>1) ^ (0xedb88320 if crc&1 else 0)
            expected.append(crc)
        self.assertEqual(table, struct.pack('<256I', *expected))

    def test_exact_layout_padding_and_crc(self):
        details = sd.inspect(self.data)
        self.assertEqual(len(self.data), 7471160)
        self.assertEqual([r['start_address'] for r in details['records']], [0x90000, 0x260000])
        self.assertEqual([r['size'] for r in details['records']], [0x1d0000, 0x550000])
        self.assertEqual([r['image_bytes'] for r in details['records']], [len(kernel()), len(rootfs())])
        self.assertEqual(struct.unpack_from('>II', self.data, 24), (zlib.crc32(self.data[32:]), len(self.data)-32))
        self.assertEqual(self.data, sd.build({2: kernel()+b'old source padding', 3: rootfs()+bytes(4096)}))

    def test_malformed_header_crc_length_truncation(self):
        for index in [0, 13, 19, 20, 24, 28, 32, 200, len(self.data)-1]:
            data = bytearray(self.data);data[index] ^= 1
            with self.subTest(index=index), self.assertRaises(ValueError):sd.inspect(data)
        for data in [b'', self.data[:-1], self.data+b'\0', self.data[192:]]:
            with self.assertRaises(ValueError):sd.inspect(data)

    def test_valid_crc_does_not_bypass_record_allowlist(self):
        # Recompute CRC so record safety checks, not checksum rejection, decide.
        records = [32, 32+12+sd.BUDGET[2]]
        for rec in records:
            for field, values in [(0,[0,1,4,5,99]), (4,[0,1,0x10000,0xffffffff]),
                                  (8,[0,0x40000,0x50000,0x90001,0x7b0000,0x800000,0xffffffff])]:
                for value in values:
                    data=bytearray(self.data);struct.pack_into('>I', data, rec+field, value)
                    with self.subTest(rec=rec,field=field,value=value), self.assertRaises(ValueError):sd.inspect(envelope(data[32:]))
        data=bytearray(self.data);struct.pack_into('>I',data,records[1],2)
        with self.assertRaises(ValueError):sd.inspect(envelope(data[32:]))
        payload=self.data[records[1]:]+self.data[32:records[1]]
        with self.assertRaises(ValueError):sd.inspect(envelope(payload))

    def test_non_ff_tails_and_invalid_embedded_images(self):
        for at in [44+len(kernel()), 44+sd.BUDGET[2]+12+len(rootfs())]:
            data=bytearray(self.data);data[at]=0
            with self.assertRaises(ValueError):sd.inspect(envelope(data[32:]))
        for at in [44+4, 44+64, 44+sd.BUDGET[2]+12, 44+sd.BUDGET[2]+12+47]:
            data=bytearray(self.data);data[at]^=1
            with self.assertRaises(ValueError):sd.inspect(envelope(data[32:]))
        for images in [{}, {2:kernel()}, {3:rootfs()}, {1:b'boot',**self.images}, {2:bytes(sd.BUDGET[2]+1),3:rootfs()}]:
            with self.assertRaises(ValueError):sd.build(images)

    def test_fake_flash_preserves_protected_and_second_boot_skips(self):
        flash=bytearray(bytes(range(256))*(sd.FLASH_SIZE//256));before=bytes(flash)
        self.assertEqual(apply_fake(flash,self.data),114)
        self.assertEqual(flash[:0x90000],before[:0x90000])
        self.assertEqual(flash[0x7b0000:],before[0x7b0000:])
        self.assertEqual(flash[0x90000:0x90000+len(kernel())],kernel())
        self.assertEqual(flash[0x260000:0x260000+len(rootfs())],rootfs())
        self.assertEqual(flash[0x90000+len(kernel()):0x260000],b'\xff'*(sd.BUDGET[2]-len(kernel())))
        self.assertEqual(flash[0x260000+len(rootfs()):0x7b0000],b'\xff'*(sd.BUDGET[3]-len(rootfs())))
        self.assertEqual(apply_fake(flash,self.data),0)
        interrupted=bytearray(before);self.assertEqual(apply_fake(interrupted,self.data,7),7)
        self.assertEqual(apply_fake(interrupted,self.data),107);self.assertEqual(interrupted,flash)

    def test_cli_build_inspect_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            temp=Path(temp);k=temp/'kernel';r=temp/'rootfs';out=temp/'linux.bin'
            k.write_bytes(kernel());r.write_bytes(rootfs())
            cmd=[sys.executable,str(ROOT/'tools/sd_package.py'),'--kernel',str(k),'--rootfs',str(r),'--output',str(out)]
            subprocess.run(cmd,check=True,capture_output=True)
            self.assertEqual(out.read_bytes(),self.data)
            self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0)
            subprocess.run([sys.executable,str(ROOT/'tools/sd_package.py'),'--inspect',str(out)],check=True,capture_output=True)


if __name__ == '__main__':unittest.main()
