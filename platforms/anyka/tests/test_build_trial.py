#!/usr/bin/env python3
"""Verify offline-only trial builds and ARM/uClibc artifact boundaries."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('trial_build', ROOT / 'tools/build_trial.py')
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)
ATTRIBUTES = 'Tag_CPU_name: "ARM926EJ-S"\nTag_CPU_arch: v5TEJ\nTag_THUMB_ISA_use: Thumb-1\n'


def shared_object(library=b'libc.so.0', *, constructors=2):
    data = bytearray(0x300)
    data[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                     3, 40, 1, 0, 52, 0x200, 0x05000200,
                     52, 32, 1, 40, 4, 0)
    struct.pack_into('<8I', data, 52, 1, 0, 0, 0, len(data), len(data), 5, 0x1000)
    strings = b'\0' + library + b'\0'
    data[0x100:0x100 + len(strings)] = strings
    struct.pack_into('<4I', data, 0x140, 1, 1, 0, 0)
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (0, 3, 2, 0x100, 0x100, len(strings), 0, 0, 1, 0),
        (0, 6, 2, 0x140, 0x140, 16, 1, 0, 4, 8),
        (0, 14, 2, 0x180, 0x180, constructors * 4, 0, 0, 4, 4),
    ]
    for index, section in enumerate(sections):
        struct.pack_into('<10I', data, 0x200 + index * 40, *section)
    return bytes(data)


class TrialBuild(unittest.TestCase):
    def test_shared_object_policy(self):
        data = shared_object()
        report = trial.inspect_shared(data, ATTRIBUTES)
        self.assertEqual(report['cpu_arch'], 'v5TEJ')
        self.assertEqual(report['needed'], ['libc.so.0'])
        self.assertEqual(report['constructor_entries'], 2)
        cases = []
        def altered(offset, fmt, value):
            out = bytearray(data)
            struct.pack_into(fmt, out, offset, value)
            return bytes(out)
        cases.extend([
            b'not an ELF', altered(18, '<H', 62), altered(16, '<H', 2),
            altered(24, '<I', 0x100), altered(36, '<I', 0x05000400),
            altered(32, '<I', 0xfffffff0), altered(52, '<I', 3),
            altered(52 + 16, '<I', len(data) + 1),
            altered(52 + 24, '<I', 4), altered(0x140, '<I', 15),
            shared_object(b'libc.so.6'), shared_object(b'libc.musl-arm.so.1'),
            shared_object(constructors=1),
        ])
        for index, invalid in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ValueError):
                trial.inspect_shared(invalid, ATTRIBUTES)
        with self.assertRaises(ValueError):
            trial.inspect_shared(data, ATTRIBUTES.replace('v5TEJ', 'v7'))
        with self.assertRaises(ValueError):
            trial.inspect_shared(data, ATTRIBUTES.replace('Thumb-1', 'Thumb-2'))

    def fixture_project(self, root):
        sources = {source for target in trial.TARGETS.values() for source in target['sources']}
        sources.update(('src/anyka/sdk_abi.h', 'src/anyka/sdk_host_guard.h',
                        'src/anyka/trial_ui.h', 'src/native/rtsp/camera_rtsp.h',
                        'src/native/control/credentials.h', 'config/sdk-host.json',
                        'tools/build_trial.py', 'tools/prepare_sdk_host.py'))
        for relative in sources:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)

    def fake_tools(self, temp):
        compiler = temp / 'arm-fixture-cc'
        encoded = base64.b64encode(shared_object()).decode()
        compiler.write_text(r'''#!/usr/bin/env python3
import base64,os,pathlib,sys
a=sys.argv[1:]
if '-dumpmachine' in a:
 print(os.environ.get('PTZ_TEST_TRIPLET','arm-test-linux-uclibcgnueabi'))
elif '-dM' in a:
 v=os.environ.get('PTZ_TEST_UCLIBC','1.0.28').split('.')
 print('#define EMPTY_MACRO\n#define __UCLIBC__ 1')
 for name,value in zip(('MAJOR','MINOR','SUBLEVEL'),v):print('#define __UCLIBC_'+name+'__ '+value)
 print('#define __ARM_ARCH 5\n#define __SOFTFP__ 1')
elif '--version' in a:print('Synthetic compiler for orchestration test only')
elif '-o' in a:
 if os.environ.get('PTZ_TEST_FAIL') and 'src/anyka/capture_trial.c' in a:
  print('injected compile failure',file=sys.stderr);sys.exit(1)
 if os.environ.get('PTZ_TEST_MUTATE'):
  pathlib.Path('src/anyka/version_probe.c').write_text('changed during build')
 pathlib.Path(a[a.index('-o')+1]).write_bytes(base64.b64decode(''' + repr(encoded) + '''))
else:sys.exit(2)
''')
        compiler.chmod(0o755)
        readelf = temp / 'fixture-readelf'
        readelf.write_text('#!/usr/bin/env python3\nprint(' + repr(ATTRIBUTES) + ')\n')
        readelf.chmod(0o755)
        return compiler, readelf

    def test_build_is_local_reproducible_and_never_executes_artifacts(self):
        with tempfile.TemporaryDirectory(prefix='ptz-trial-build-') as temp:
            temp = Path(temp)
            root = temp / 'checkout'
            self.fixture_project(root)
            compiler, readelf = self.fake_tools(temp)
            real_run = subprocess.run
            called = []
            def controlled(command, *args, **kwargs):
                # The only allowed processes are compiler and host ELF inspector.
                self.assertIn(str(command[0]), (str(compiler), str(readelf)))
                called.append(list(map(str, command)))
                return real_run(command, *args, **kwargs)
            with patch.object(trial, 'ROOT', root), patch.object(trial.subprocess, 'run', side_effect=controlled):
                first = trial.build(str(compiler), str(readelf), 'all', root / 'output/one')
                second = trial.build(str(compiler), str(readelf), 'all', root / 'output/two')
            self.assertEqual(first, second)
            self.assertTrue(called)
            self.assertFalse((root / 'investigation').exists())
            self.assertTrue(first['experimental'])
            self.assertFalse(first['firmware'])
            self.assertFalse(first['camera_contacted_by_build'])
            self.assertFalse(first['target_executed_by_build'])
            self.assertFalse(first['vendor_host_included'])
            self.assertEqual(set(first['artifacts']), {'version-probe.so', 'capture-trial.so', 'rtsp-av-trial.so'})
            for name, report in first['artifacts'].items():
                data = (root / 'output/one' / name).read_bytes()
                self.assertEqual(report['sha256'], hashlib.sha256(data).hexdigest())
                self.assertEqual(data, (root / 'output/two' / name).read_bytes())
            self.assertIn('src/anyka/audio_backend.c', first['artifacts']['rtsp-av-trial.so']['sources'])
            self.assertIn('src/anyka/trial_control.c', first['artifacts']['rtsp-av-trial.so']['sources'])

    def test_wrong_toolchain_and_failed_build_publish_nothing(self):
        with tempfile.TemporaryDirectory(prefix='ptz-trial-refusal-') as temp:
            temp = Path(temp)
            root = temp / 'checkout'
            self.fixture_project(root)
            compiler, readelf = self.fake_tools(temp)
            output = root / 'output/trial'
            for environment in ({'PTZ_TEST_TRIPLET': 'x86_64-linux-gnu'},
                                {'PTZ_TEST_UCLIBC': '1.0.42'}, {'PTZ_TEST_FAIL': '1'},
                                {'PTZ_TEST_MUTATE': '1'}):
                with self.subTest(environment=environment), patch.object(trial, 'ROOT', root), patch.dict(os.environ, environment):
                    with self.assertRaises(ValueError):
                        trial.build(str(compiler), str(readelf), 'all', output)
                    self.assertFalse(output.exists())
            with patch.object(trial, 'ROOT', root), self.assertRaises(ValueError):
                trial.build(str(compiler), str(readelf), 'all', root / 'src')


if __name__ == '__main__':
    unittest.main()
