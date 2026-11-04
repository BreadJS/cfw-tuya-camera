#!/usr/bin/env python3
"""Build-policy tests; does not execute target artifacts or access a camera."""
import importlib.util
from pathlib import Path
import sys
import struct
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_application

class Policy(unittest.TestCase):
    def test_output_escape_is_rejected_before_compiler(self):
        with patch.object(build_application, 'inspect_toolchain') as compiler:
            with self.assertRaisesRegex(ValueError, 'output/'):
                build_application.build('/no/compiler', 'readelf', Path('/tmp/not-project-output'))
            compiler.assert_not_called()

    def test_source_inventory_contains_actual_application(self):
        sources = build_application.SOURCES
        self.assertEqual(len(sources), len(set(sources)))
        self.assertIn('src/anyka/application.c', sources)
        self.assertIn('src/anyka/reset_button.c', sources)
        self.assertIn('src/anyka/update_http.c', sources)
        self.assertNotIn('src/anyka/rtsp_trial.c', sources)
        for source in sources:
            self.assertTrue((ROOT / source).is_file(), source)

    @staticmethod
    def helper():
        data = bytearray(256)
        data[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHIII', data, 16, 2, 40, 1, 0x10080, 52)
        struct.pack_into('<IHHH', data, 36, 0x05000200, 52, 32, 2)
        struct.pack_into('<8I', data, 52, 1, 0, 0x10000, 0, 256, 256, 5, 4096)
        interpreter = b'/lib/ld-uClibc.so.0\0'
        struct.pack_into('<8I', data, 84, 3, 200, 0x100c8, 0, len(interpreter), len(interpreter), 4, 1)
        data[200:200+len(interpreter)] = interpreter
        return data

    def test_clock_and_login_executable_policy(self):
        attrs = 'Tag_CPU_arch: v5TE'
        dynamic = 'Shared library: [libc.so.0]'
        report = build_application.inspect_executable(self.helper(), attrs, dynamic)
        self.assertEqual(report['interpreter'], '/lib/ld-uClibc.so.0')
        malformed = self.helper()
        # A correct-looking interpreter elsewhere does not excuse wrong PT_INTERP.
        malformed[200] = ord('X')
        with self.assertRaisesRegex(ValueError, 'interpreter'):
            build_application.inspect_executable(malformed, attrs, dynamic)
        with self.assertRaisesRegex(ValueError, 'runtime dependencies'):
            build_application.inspect_executable(self.helper(), attrs, dynamic+' Shared library: [libforeign.so]')
        with self.assertRaisesRegex(ValueError, 'architecture'):
            build_application.inspect_executable(self.helper(), attrs+' Tag_THUMB_ISA_use: Thumb-2', dynamic)
        malformed = self.helper()
        struct.pack_into('<I', malformed, 24, 0x80000000)
        with self.assertRaisesRegex(ValueError, 'mapped executable'):
            build_application.inspect_executable(malformed, attrs, dynamic)

if __name__ == '__main__':
    unittest.main()
