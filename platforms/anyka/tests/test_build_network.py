#!/usr/bin/env python3
"""Source pin tests; no download, target execution or network operation."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
spec=importlib.util.spec_from_file_location('network',ROOT/'tools/build_network.py')
network=importlib.util.module_from_spec(spec);spec.loader.exec_module(network)


class Sources(unittest.TestCase):
    def test_tree_hash_changes_for_name_and_contents_not_git_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree=Path(tmp);(tree/'a').write_text('one')
            before=network.tree_hash(tree)
            (tree/'.git').mkdir();(tree/'.git/config').write_text('local machine secret')
            self.assertEqual(before,network.tree_hash(tree))
            (tree/'a').write_text('two')
            self.assertNotEqual(before,network.tree_hash(tree))
            before=network.tree_hash(tree);(tree/'a').rename(tree/'b')
            self.assertNotEqual(before,network.tree_hash(tree))

    def test_symlink_source_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree=Path(tmp);(tree/'escape').symlink_to('/etc/passwd')
            with self.assertRaises(ValueError):network.tree_hash(tree)

    def test_primary_sources_and_hashes_are_explicit(self):
        pin=json.loads((ROOT/'config/network-sources.json').read_text())
        self.assertEqual(pin['hostapd']['url'],'https://w1.fi/releases/hostapd-2.12.tar.gz')
        self.assertEqual(len(pin['hostapd']['sha256']),64)
        self.assertEqual(len(pin['libnl_tiny']['commit']),40)
        self.assertEqual(len(pin['libnl_tiny']['tree_sha256']),64)


if __name__=='__main__':unittest.main()
