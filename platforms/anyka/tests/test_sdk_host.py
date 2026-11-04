#!/usr/bin/env python3
"""Exercise ELF mapping policy and preload termination without vendor binaries."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sdk_host', ROOT / 'tools/prepare_sdk_host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
ORIGINAL = bytes.fromhex('00482de904b08de216dd4de260050be5')


def fixture():
    data = bytearray(0x2a0)
    data[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                     2, 40, 1, 0x10100, 52, 0x200, 0x05000200,
                     52, 32, 1, 40, 4, 0)
    struct.pack_into('<8I', data, 52, 1, 0, 0x10000, 0x10000,
                     len(data), len(data), 5, 0x1000)
    data[0x100:0x110] = ORIGINAL
    struct.pack_into('<IIIBBH', data, 0x1a0, 1, 0x10100, 64, 0x12, 0, 1)
    data[0x1c0:0x1c6] = b'\0main\0'
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (0, 1, 6, 0x10100, 0x100, 64, 0, 0, 4, 0),
        (0, 11, 2, 0x10190, 0x190, 32, 3, 1, 4, 16),
        (0, 3, 2, 0x101c0, 0x1c0, 6, 0, 0, 1, 0),
    ]
    for index, section in enumerate(sections):
        struct.pack_into('<10I', data, 0x200 + index * 40, *section)
    data = bytes(data)
    result = data[:0x100] + host.STUB + data[0x110:]
    policy = {'schema': 1, 'board': 'synthetic-test-only',
              'source_sha256': hashlib.sha256(data).hexdigest(), 'source_bytes': len(data),
              'elf_entry': 0x10100, 'main_symbol': 'main', 'main_address': 0x10100,
              'main_file_offset': 0x100, 'main_original_hex': ORIGINAL.hex(),
              'fallback_exit': 125, 'sdk_host_sha256': hashlib.sha256(result).hexdigest()}
    return data, policy


class SDKHost(unittest.TestCase):
    def test_only_main_patch_span_changes(self):
        data, policy = fixture()
        result, report = host.prepare(data, policy)
        self.assertEqual(result[:0x100], data[:0x100])
        self.assertEqual(result[0x110:], data[0x110:])
        self.assertEqual(result[0x100:0x110], host.STUB)
        self.assertEqual(len(result), len(data))
        self.assertTrue(report['proprietary_code_retained'])
        self.assertFalse(report['flash_image'])
        self.assertFalse(report['target_executed_by_preparer'])
        self.assertEqual(report['fallback_exit'], 125)

    def test_wrong_revision_and_patch_mapping_are_rejected(self):
        data, policy = fixture()
        altered = bytearray(data)
        altered[0x120] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA256'):
            host.prepare(bytes(altered), policy)
        for field, value in [('main_address', 0x10104), ('main_file_offset', 0x104),
                             ('elf_entry', 0x10104), ('main_original_hex', '00' * 16),
                             ('sdk_host_sha256', '00' * 32), ('fallback_exit', 0)]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                host.prepare(data, dict(policy, **{field: value}))
        with self.assertRaisesRegex(ValueError, 'size'):
            host.prepare(data[:-1], policy)

    def test_elf_policy_rejects_non_arm_missing_main_and_bad_mapping(self):
        data, _ = fixture()
        cases = []
        def modify(offset, fmt, value):
            out = bytearray(data)
            struct.pack_into(fmt, out, offset, value)
            return bytes(out)
        cases.extend([
            modify(18, '<H', 3),       # non-ARM
            modify(16, '<H', 3),       # ET_DYN, not reviewed ET_EXEC
            modify(36, '<I', 0x05000400),  # hard-float
            modify(32, '<I', 0xfffffff0),  # section table outside file
            modify(0x1a0, '<I', 0xffff),   # bad symbol string offset
            modify(0x1a4, '<I', 0x10101),  # Thumb/unaligned main
            modify(52 + 24, '<I', 4),     # mapped segment not executable
            modify(0x200 + 40 + 16, '<I', 0x104),  # section/segment mismatch
            modify(0x200 + 80 + 24, '<I', 2),     # wrong dynstr section
        ])
        changed = bytearray(data)
        changed[0x1c1:0x1c5] = b'nope'
        cases.append(bytes(changed))
        for index, malformed in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ValueError):
                host.main_location(malformed)
        with self.assertRaises(ValueError):
            host.main_location(b'\x7fELF')

    def test_cli_bad_source_creates_no_host(self):
        with tempfile.TemporaryDirectory(prefix='ptz-sdk-host-reject-') as temp:
            source = Path(temp) / 'unknown-executable'
            source.write_bytes(b'not a reviewed Anyka executable')
            out = ROOT / 'output' / ('rejected-sdk-' + Path(temp).name)
            result = subprocess.run(['python3', str(ROOT / 'tools/prepare_sdk_host.py'),
                                     '--source', str(source), '--output-dir', str(out)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('not prepared', result.stderr)
            self.assertFalse(out.exists())

    def test_output_location_is_private_or_ignored(self):
        self.assertTrue(host.allowed_output(ROOT / 'base/sdk'))
        self.assertTrue(host.allowed_output(ROOT / 'output/sdk-probe'))
        self.assertFalse(host.allowed_output(ROOT / 'src/anyka'))
        self.assertFalse(host.allowed_output(ROOT.parent / 'other-project'))

    def test_public_policy_keeps_reviewed_identity(self):
        policy = json.loads((ROOT / 'config/sdk-host.json').read_text())
        self.assertEqual(policy['source_sha256'], 'e2a2f0d484fb5c79e99521037397719574633f1c0f306f70a728a7ee72083c7e')
        self.assertEqual(policy['main_address'], 0x9f8b0)
        self.assertEqual(policy['main_file_offset'], 0x8f8b0)
        self.assertEqual(struct.unpack('<4I', host.STUB),
                         (0xe3a0007d, 0xe3a07001, 0xef000000, 0xeafffffe))

    def test_version_constructor_exits_before_main_and_handles_missing_symbol(self):
        names = ['ak_vi_get_version', 'ak_venc_get_version', 'ak_ai_get_version',
                 'ak_aenc_get_version', 'ak_vpss_get_version', 'ak_mem_get_version',
                 'ak_app_video_get_version']
        with tempfile.TemporaryDirectory(prefix='ptz-version-constructor-') as temp:
            temp = Path(temp)
            preload = temp / 'probe.so'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-DCAMERA_VERSION_PROBE_HOST_TEST',
                            str(ROOT / 'src/anyka/version_probe.c'), '-ldl', '-o', str(preload)], check=True)
            for missing in (False, True):
                functions = names[:-1] if missing else names
                source = temp / 'host.c'
                source.write_text('#include <stdio.h>\n' + '\n'.join(
                    f'const char *{name}(void){{return "synthetic version";}}' for name in functions)
                    + '\nint main(void){puts("ORIGINAL_MAIN_RAN");return 99;}\n')
                executable = temp / 'host'
                subprocess.run(['cc', '-std=c11', '-Werror', '-rdynamic', str(source), '-o', str(executable)], check=True)
                import os
                environment = dict(os.environ, LD_PRELOAD=str(preload))
                result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True)
                self.assertEqual(result.returncode, 3 if missing else 0)
                self.assertNotIn('ORIGINAL_MAIN_RAN', result.stdout)
                self.assertIn('vendor_main_called=no', result.stdout)
                self.assertEqual(result.stdout.count('SDK_PROBE function='), 6 if missing else 7)
                if missing:
                    self.assertIn('missing=ak_app_video_get_version', result.stdout)

    def test_runtime_guard_checks_elf_mapping_and_exact_stub(self):
        with tempfile.TemporaryDirectory(prefix='ptz-sdk-guard-') as temp:
            temp = Path(temp)
            driver = temp / 'driver.c'
            driver.write_text('#define _GNU_SOURCE\n#define _FILE_OFFSET_BITS 64\n'
                              '#include "sdk_host_guard.h"\n'
                              'int main(int n,char **v){if(n!=2)return 2;'
                              'int fd=open(v[1],O_RDONLY);if(fd<0)return 2;'
                              'int rc=camera_sdk_host_guard_fd(fd);close(fd);return rc?2:0;}\n')
            executable = temp / 'guard'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-I' + str(ROOT / 'src/anyka'), str(driver), '-o', str(executable)], check=True)
            data = bytearray(0x8f8c0)
            data[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
            struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                             2, 40, 1, 0x5f970, 52, 0, 0x05000200,
                             52, 32, 1, 40, 0, 0)
            struct.pack_into('<8I', data, 52, 1, 0, 0x10000, 0x10000,
                             len(data), len(data), 5, 0x1000)
            data[0x8f8b0:0x8f8c0] = host.STUB
            sample = temp / 'host'
            sample.write_bytes(data)
            subprocess.run([str(executable), str(sample)], check=True)
            cases = []
            def altered(offset, fmt, value):
                changed = bytearray(data)
                struct.pack_into(fmt, changed, offset, value)
                return changed
            cases.extend([
                data[:51],
                altered(18, '<H', 3),                 # wrong architecture
                altered(16, '<H', 3),                 # ET_DYN
                altered(24, '<I', 0),                 # wrong reviewed entry
                altered(28, '<I', 0xfffffff0),         # unbounded header table
                altered(36, '<I', 0x05000400),         # hard float
                altered(44, '<H', 0),                 # no load headers
                altered(44, '<H', 33),                # too many headers
                altered(52 + 16, '<I', len(data) + 1), # load beyond file
                altered(52 + 24, '<I', 4),            # no executable mapping
                altered(0x8f8b0, '<I', 0xe92d4800),    # original main, no stub
            ])
            for index, malformed in enumerate(cases):
                with self.subTest(case=index):
                    sample.write_bytes(malformed)
                    result = subprocess.run([str(executable), str(sample)], capture_output=True)
                    self.assertEqual(result.returncode, 2, result.stderr.decode())

    def test_native_unprepared_host_refuses_before_any_getter(self):
        import os
        with tempfile.TemporaryDirectory(prefix='ptz-sdk-guard-refusal-') as temp:
            temp = Path(temp)
            source = temp / 'host.c'
            source.write_text('#include <stdio.h>\n'
                              'const char *ak_vi_get_version(void){puts("GETTER_CALLED");return "test";}\n'
                              'int main(void){puts("ORIGINAL_MAIN_RAN");return 99;}\n')
            executable = temp / 'host'
            subprocess.run(['cc', '-std=c11', '-Werror', '-rdynamic', str(source), '-o', str(executable)], check=True)
            for flags in ([], ['-D__arm__=1', '-DCAMERA_VERSION_PROBE_HOST_TEST']):
                preload = temp / 'probe.so'
                subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                                *flags, str(ROOT / 'src/anyka/version_probe.c'), '-ldl', '-o', str(preload)], check=True)
                result = subprocess.run([str(executable)], env=dict(os.environ, LD_PRELOAD=str(preload)),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 125)
                self.assertIn('host_guard_failed no_sdk_calls=yes', result.stdout)
                self.assertNotIn('GETTER_CALLED', result.stdout)
                self.assertNotIn('ORIGINAL_MAIN_RAN', result.stdout)


if __name__ == '__main__':
    unittest.main()
