#!/usr/bin/env python3
"""Verify build independence and prevent incomplete firmware publication."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('public_sources', ROOT / 'tools/source_files.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class Repository(unittest.TestCase):
    def test_clean_checkout_builds_without_research_or_sibling_project(self):
        with tempfile.TemporaryDirectory(prefix='ptz-clean-checkout-') as temp:
            root = Path(temp) / 'checkout'
            for source in inventory.source_files():
                target = root / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            subprocess.run(['python3', 'tools/check_repo.py'], cwd=root, check=True)
            subprocess.run(['python3', 'tools/build_core.py', '--host'], cwd=root, check=True)
            self.assertTrue((root / 'output/host/libcamera-core.a').is_file())
            self.assertFalse((root / 'investigation').exists())
            report = json.loads((root / 'output/host/manifest.json').read_text())
            self.assertFalse(report['flashable'])
            # Readiness declarations cannot supply missing build inputs.
            readiness = root / 'config/readiness.json'
            data = json.loads(readiness.read_text())
            data.update({key: True for key in data if key != 'schema'})
            readiness.write_text(json.dumps(data))
            p = subprocess.run(['python3', 'tools/build_release.py', '--target', 'both'],
                               cwd=root, capture_output=True, text=True)
            self.assertEqual(p.returncode, 2)
            self.assertIn('missing actual artifacts', p.stdout + p.stderr)
            self.assertFalse((root / 'output/release').exists())

    def test_incomplete_releases_produce_no_firmware(self):
        for target in ('web', 'sd', 'both'):
            with self.subTest(target=target), tempfile.TemporaryDirectory(prefix='ptz-release-refusal-') as temp:
                output = Path(temp) / 'release'
                p = subprocess.run(['python3', str(ROOT / 'tools/build_release.py'),
                                    '--target', target, '--check', '--output-dir', str(output)],
                                   capture_output=True, text=True)
                self.assertEqual(p.returncode, 2)
                self.assertIn('No package generated', p.stdout + p.stderr)
                self.assertFalse(output.exists())

    def test_source_export_excludes_research_vendor_outputs_and_keys(self):
        with tempfile.TemporaryDirectory(prefix='ptz-source-archive-') as temp:
            out = Path(temp) / 'source.zip'
            subprocess.run(['python3', str(ROOT / 'tools/export_source.py'), '--output', str(out)], check=True)
            with zipfile.ZipFile(out) as archive:
                self.assertIsNone(archive.testzip())
                names = archive.namelist()
                self.assertIn('compile-sd.sh', names)
                self.assertIn('src/native/rtsp/camera_rtsp.c', names)
                for name in names:
                    self.assertFalse(name.startswith(('investigation/', 'base/vendor/', 'base/sdk/', 'output/')))
                    self.assertFalse(name.endswith(('.pem', '.key', '.bin', '.cap')))

    def test_public_inventory_rejects_accidental_private_or_binary_files(self):
        with tempfile.TemporaryDirectory(prefix='ptz-source-guard-') as temp:
            root = Path(temp)
            private = root / 'private.pem'
            private.write_text('test signing key placeholder')
            with self.assertRaises(ValueError):
                list(inventory.source_files(root))
            private.unlink()
            (root / 'disguised.txt').write_bytes(b'\x7fELFtest')
            with self.assertRaises(ValueError):
                list(inventory.source_files(root))


if __name__ == '__main__':
    unittest.main()
