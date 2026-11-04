#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('app',ROOT/'tools/build_app.py')
app=importlib.util.module_from_spec(spec);spec.loader.exec_module(app)


class Image(unittest.TestCase):
    def test_exact_partition_and_canonical_tail(self):
        source=bytearray(4096);source[:4]=b'hsqs';struct.pack_into('<Q',source,40,120)
        canonical,padded=app.pad_partition(source)
        self.assertEqual(canonical,source[:120]);self.assertEqual(len(padded),5242880)
        self.assertEqual(padded[120:],b'\xff'*(5242880-120))

    def test_size_and_non_squashfs_are_refused(self):
        for used in (0,95,4097,0x500001):
            data=bytearray(4096);data[:4]=b'hsqs';struct.pack_into('<Q',data,40,used)
            with self.assertRaises(ValueError):app.pad_partition(data)
        with self.assertRaises(ValueError):app.pad_partition(b'not firmware')

    def test_loop_image_is_block_aligned_without_altering_partition(self):
        for used in (120,4096,4097,3952978,app.PARTITION_BYTES):
            source=bytearray(used);source[:4]=b'hsqs';struct.pack_into('<Q',source,40,used)
            canonical,partition=app.pad_partition(source)
            loop=app.pad_loop(source)
            self.assertEqual(len(loop)%4096,0)
            self.assertGreaterEqual(len(loop),used)
            self.assertLess(len(loop)-used,4096)
            self.assertEqual(loop[:used],canonical)
            self.assertEqual(loop[used:],b'\xff'*(len(loop)-used))
            self.assertEqual(partition,canonical+b'\xff'*(app.PARTITION_BYTES-used))
        # Reproduces the observed old-loop-device truncation condition.
        self.assertEqual(3952978//512*512,3952640)
        self.assertGreater(3952978,3952978//512*512)
        self.assertEqual(len(app.pad_loop(source)),app.PARTITION_BYTES)

    def test_host_and_hard_float_artifacts_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            file=Path(directory)/'binary';file.write_bytes(Path('/bin/true').read_bytes())
            with self.assertRaises(ValueError):app.arm_elf(file,2)
            data=bytearray(52);data[:7]=b'\x7fELF\x01\x01\x01';struct.pack_into('<HH',data,16,2,40);struct.pack_into('<I',data,36,0x05000400);file.write_bytes(data)
            with self.assertRaises(ValueError):app.arm_elf(file,2)

    def test_baseline_initialization_never_repairs_existing_journals(self):
        import subprocess
        helper=ROOT/'rootfs-overlay/usr/libexec/camera/init-update-state'
        with tempfile.TemporaryDirectory() as directory:
            state=Path(directory)/'update'
            subprocess.run(['sh',str(helper),str(state)],check=True)
            self.assertEqual((state/'accepted').read_text(),'0\n')
            (state/'accepted').write_text('27\n')
            (state/'pending.json').write_text('broken evidence must survive')
            subprocess.run(['sh',str(helper),str(state)],check=True)
            self.assertEqual((state/'accepted').read_text(),'27\n')
            (state/'accepted').unlink()
            subprocess.run(['sh',str(helper),str(state)],check=True)
            self.assertFalse((state/'accepted').exists())
            self.assertEqual((state/'pending.json').read_text(),'broken evidence must survive')
            link=Path(directory)/'link';link.symlink_to(state)
            self.assertNotEqual(subprocess.run(['sh',str(helper),str(link)]).returncode,0)

    def test_overlay_never_runs_vendor_main_and_stages_to_ram(self):
        boot=(ROOT/'rootfs-overlay/usr/sbin/main.sh').read_text()
        self.assertIn('cp -R /usr/libexec/camera/.',boot)
        self.assertIn('exec /bin/sh "$runtime/boot"',boot)
        runtime=(ROOT/'rootfs-overlay/usr/libexec/camera/boot').read_text()
        self.assertIn('exec /tmp/camera-runtime/sdk-host',runtime)
        for path in (ROOT/'rootfs-overlay/usr').rglob('*'):
            if path.is_file():
                import subprocess
                subprocess.run(['sh','-n',str(path)],check=True)


if __name__=='__main__':unittest.main()
