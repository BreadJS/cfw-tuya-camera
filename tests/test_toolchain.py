"""Bundled SDK installation must work offline and fail closed on bad inputs."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import toolchain
from export_source import source_files
from profiles import ROOT, ProfileError, load


class Toolchain(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / 'toolchains/test.tar.gz'
        self.archive.parent.mkdir()
        with tarfile.open(self.archive, 'w:gz') as archive:
            member = tarfile.TarInfo('sdk/bin/gcc')
            data = b'#!/bin/sh\nexit 0\n'
            member.size = len(data)
            member.mode = 0o755
            archive.addfile(member, io.BytesIO(data))
        self.pin = {
            'archive': 'toolchains/test.tar.gz',
            'bytes': self.archive.stat().st_size,
            'sha256': hashlib.sha256(self.archive.read_bytes()).hexdigest(),
            'compiler_relative': 'sdk/bin/gcc',
        }
        self.profile = {'soc': {'id': 'test-soc', 'backend': 'test', 'toolchain': self.pin}}
        self.cache = self.root / 'cache'
        self.root_patch = patch.object(toolchain, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        network = patch('socket.socket', side_effect=AssertionError('Network access is forbidden'))
        network.start()
        self.addCleanup(network.stop)

    def test_offline_install_and_reuse(self):
        cc = toolchain.install(self.profile, self.cache)
        self.assertTrue(cc.is_file())
        self.assertEqual(cc.stat().st_mode & 0o777, 0o755)
        self.assertEqual(toolchain.install(self.profile, self.cache), cc)
        self.assertEqual(json.loads((self.root / 'local/toolchains.json').read_text()), {'test': str(cc)})

    def test_missing_archive_never_falls_back_to_cached_install(self):
        toolchain.install(self.profile, self.cache)
        self.archive.unlink()
        with self.assertRaisesRegex(ProfileError, 'archive is missing'):
            toolchain.install(self.profile, self.cache)

    def test_corrupt_archive_is_rejected_before_extraction(self):
        data = self.archive.read_bytes()
        self.archive.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        with self.assertRaisesRegex(ProfileError, 'differs from pin'):
            toolchain.install(self.profile, self.cache)
        self.assertFalse(self.cache.exists())

    def test_archive_outside_repository_is_rejected(self):
        self.pin['archive'] = '../test.tar.gz'
        with self.assertRaisesRegex(ProfileError, 'inside the repository'):
            toolchain.install(self.profile, self.cache)

    def test_incomplete_install_is_rejected(self):
        (self.cache / 'test-soc').mkdir(parents=True)
        with self.assertRaisesRegex(ProfileError, 'completion receipt'):
            toolchain.install(self.profile, self.cache)

    def test_shipped_archives_match_pins_and_are_exported(self):
        exported = set(source_files(ROOT))
        for board in ('lsc-smart-indoor', 'lsc-smart-rotatable'):
            with self.subTest(board=board):
                pin = load(board)['soc']['toolchain']
                self.assertNotIn('url', pin)
                archive = ROOT / pin['archive']
                self.assertIn(archive, exported)
                self.assertEqual(archive.stat().st_size, pin['bytes'])
                self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), pin['sha256'])


if __name__ == '__main__':
    unittest.main()
