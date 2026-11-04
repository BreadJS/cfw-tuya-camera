#!/usr/bin/env python3
"""Build experimental ARMv5 SDK-host preload trials, without camera access.

The artifacts are shared objects, not firmware or standalone executables. This
tool never executes them, contacts a camera, obtains vendor binaries, or installs
anything. Runtime use requires the separately prepared, SHA-pinned SDK host and
the documented exclusive-owner/watchdog safeguards.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from prepare_sdk_host import checked_range, unpack

ROOT = Path(__file__).resolve().parents[1]
ARM_FLAGS = ['-mcpu=arm926ej-s', '-marm', '-mfloat-abi=soft']
FLAGS = ['-std=c11', '-Os', '-fPIC', '-shared', '-pthread', '-Wall', '-Wextra', '-Werror',
         *ARM_FLAGS, '-Wl,--hash-style=sysv', '-Wl,--build-id=none', '-Wl,--entry=0']
TARGETS = {
    'version': {'file': 'version-probe.so', 'define': None,
                'sources': ['src/anyka/version_probe.c'],
                'hardware_when_loaded': False, 'network_when_loaded': False},
    'capture': {'file': 'capture-trial.so', 'define': 'CAMERA_ANYKA_CAPTURE_TRIAL_CONSTRUCTOR',
                'sources': ['src/anyka/capture_trial.c', 'src/anyka/sdk_api.c'],
                'hardware_when_loaded': True, 'network_when_loaded': False},
    'rtsp': {'file': 'rtsp-av-trial.so', 'define': 'CAMERA_ANYKA_RTSP_TRIAL_CONSTRUCTOR',
             'sources': ['src/anyka/rtsp_trial.c', 'src/anyka/media_backend.c', 'src/anyka/sensor_timing.c', 'src/anyka/video_mode.c',
                         'src/anyka/audio_backend.c', 'src/anyka/sdk_api.c',
                         'src/anyka/trial_guard.c', 'src/anyka/trial_control.c',
                         'src/native/rtsp/camera_rtsp.c', 'src/native/rtsp/h264_parameters.c', 'src/native/control/http.c',
                         'src/preview/preview.c', 'src/preview/mp4.c'],
             'hardware_when_loaded': True, 'network_when_loaded': True},
}
ALLOWED_LIBRARIES = {'libc.so.0', 'libdl.so.0', 'libpthread.so.0', 'librt.so.0', 'libgcc_s.so.1'}


def run(args, *, input=None):
    result = subprocess.run([str(arg) for arg in args], input=input, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, LC_ALL='C'))
    if result.returncode:
        raise ValueError('Command failed: ' + str(args[0]) + '\n' + result.stderr[-5000:])
    return result.stdout


def inspect_toolchain(cc):
    triplet = run([cc, '-dumpmachine']).strip()
    if not triplet.startswith('arm') or 'uclibc' not in triplet:
        raise ValueError('Trials require an ARM uClibc toolchain, not a host or musl compiler')
    text = run([cc, *ARM_FLAGS, '-dM', '-E', '-include', 'features.h', '-'], input='')
    macros = dict(re.findall(r'^#define[ \t]+(\w+)[ \t]+([^\n]*)$', text, re.M))
    actual = tuple(macros.get(name, '').strip() for name in
                   ('__UCLIBC_MAJOR__', '__UCLIBC_MINOR__', '__UCLIBC_SUBLEVEL__'))
    if actual != ('1', '0', '28'):
        raise ValueError('Trials require the reviewed uClibc1.0.28 ABI; found ' + '.'.join(actual))
    if macros.get('__ARM_ARCH', '').strip() != '5' or '__SOFTFP__' not in macros or '__ARM_PCS_VFP' in macros:
        raise ValueError('Compiler did not select ARMv5 soft-float mode')
    return {'triplet': triplet, 'uclibc': '1.0.28',
            'version': run([cc, '--version']).splitlines()[0]}


def inspect_shared(data, attributes):
    header = checked_range(data, 0, 52, 'ELF header')
    if header[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Trial is not ELF32 little-endian')
    (kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize,
     phnum, shsize, shnum, _) = struct.unpack_from('<HHIIIIIHHHHHH', header, 16)
    if (kind, machine, version, entry) != (3, 40, 1, 0):
        raise ValueError('Trial must be an ARM shared object with no program entry point')
    if flags & 0xff000000 != 0x05000000 or flags & 0x600 != 0x200:
        raise ValueError('Trial is not marked EABI5 soft-float')
    if ehsize != 52 or phsize != 32 or shsize != 40 or not phnum or not shnum:
        raise ValueError('Unexpected shared-object ELF table layout')
    checked_range(data, phoff, phnum * phsize, 'program table')
    checked_range(data, shoff, shnum * shsize, 'section table')
    have_code = False
    for index in range(phnum):
        ph = unpack(data, '<8I', phoff + index * phsize, 'program header')
        if ph[0] == 3:
            raise ValueError('Trial unexpectedly contains a program interpreter')
        if ph[0] == 1:
            checked_range(data, ph[1], ph[4], 'load segment')
            if ph[4] > ph[5]:
                raise ValueError('Invalid shared-object segment size')
            have_code |= bool(ph[6] & 1 and ph[4])
    if not have_code:
        raise ValueError('Trial has no executable load segment')
    sections = [unpack(data, '<10I', shoff + index * shsize, 'section') for index in range(shnum)]
    needed = []
    constructors = 0
    for section in sections:
        if section[1] == 14:  # SHT_INIT_ARRAY
            checked_range(data, section[4], section[5], 'constructor array')
            if section[5] % 4:
                raise ValueError('Invalid constructor array size')
            constructors += section[5] // 4
        if section[1] != 6:  # SHT_DYNAMIC
            continue
        if section[6] >= len(sections) or section[5] % 8:
            raise ValueError('Invalid dynamic section')
        strings_section = sections[section[6]]
        if strings_section[1] != 3:
            raise ValueError('Dynamic section has no string table')
        strings = checked_range(data, strings_section[4], strings_section[5], 'dynamic strings')
        for offset in range(section[4], section[4] + section[5], 8):
            tag, value = unpack(data, '<II', offset, 'dynamic entry')
            if tag in (15, 29):
                raise ValueError('Trial must not embed build-machine library search paths')
            if tag != 1:
                continue
            end = strings.find(b'\0', value)
            if value >= len(strings) or end < 0:
                raise ValueError('Invalid library string')
            needed.append(strings[value:end].decode('ascii'))
    if 'libc.so.0' not in needed or set(needed) - ALLOWED_LIBRARIES:
        raise ValueError('Trial uses an unexpected runtime: ' + ','.join(needed))
    if constructors < 2:
        raise ValueError('Trial constructor is missing')
    cpu = re.search(r'Tag_CPU_arch:\s*(\S+)', attributes)
    if not cpu or cpu[1] not in ('v5TE', 'v5TEJ') or 'Tag_THUMB_ISA_use: Thumb-2' in attributes:
        raise ValueError('Trial ELF attributes are not ARMv5TE/ARMv5TEJ')
    return {'machine': 'ARM', 'cpu_arch': cpu[1], 'elf': 'ELF32-little-ET_DYN',
            'abi': 'EABI5-soft', 'needed': sorted(needed), 'constructor_entries': constructors}


def build(cc, readelf, targets, output):
    if not output.resolve().is_relative_to((ROOT / 'output').resolve()):
        raise ValueError('Trial output must remain under this project output/')
    chosen = list(TARGETS) if targets == 'all' else [targets]
    for name in chosen:
        for source in TARGETS[name]['sources']:
            if not (ROOT / source).is_file():
                raise ValueError('Missing local source: ' + source)
    compiler = inspect_toolchain(cc)
    # Pin the actual compilation inputs before building. Concurrent edits must
    # cause refusal rather than a manifest describing different source bytes.
    inputs = {source for name in chosen for source in TARGETS[name]['sources']}
    for directory in ('src/anyka', 'src/native/rtsp', 'src/native/control', 'src/preview'):
        inputs.update(str(p.relative_to(ROOT)) for p in (ROOT / directory).glob('*.h')
                      if p.name != 'ui.h')
    inputs.update(('config/sdk-host.json', 'tools/build_trial.py', 'tools/prepare_sdk_host.py'))
    source_hashes = {source: hashlib.sha256((ROOT / source).read_bytes()).hexdigest()
                     for source in sorted(inputs)}
    output.parent.mkdir(parents=True, exist_ok=True)
    policy = json.loads((ROOT / 'config/sdk-host.json').read_text())
    manifest = {'schema': 1, 'experimental': True, 'firmware': False,
                'standalone_camera_application': False, 'vendor_host_included': False,
                'camera_contacted_by_build': False, 'target_executed_by_build': False,
                'flash_modified': False, 'board': policy['board'],
                'required_sdk_host_sha256': policy['sdk_host_sha256'],
                'compiler': compiler, 'flags': FLAGS, 'artifacts': {}, 'sources': source_hashes}
    with tempfile.TemporaryDirectory(prefix='.sdk-trial-build-', dir=output.parent) as temp:
        temp = Path(temp)
        for name in chosen:
            target = TARGETS[name]
            artifact = temp / target['file']
            defines = ['-D' + target['define']] if target['define'] else []
            if name == 'rtsp':
                defines.append('-DCAMERA_CONTROL_UI_HEADER="../../anyka/trial_ui.h"')
            # Relative input paths keep __FILE__ stable across checkout locations.
            command = [cc, *FLAGS, *defines, *target['sources'], '-ldl', '-o', str(artifact)]
            result = subprocess.run(command, cwd=ROOT, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    env=dict(os.environ, LC_ALL='C'))
            if result.returncode:
                raise ValueError(name + ' compilation failed:\n' + result.stderr[-5000:])
            data = artifact.read_bytes()
            attributes = run([readelf, '-A', artifact])
            report = inspect_shared(data, attributes)
            report.update({'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                           'sources': target['sources'],
                           'defines': defines,
                           'hardware_access_when_loaded': target['hardware_when_loaded'],
                           'network_service_when_loaded': target['network_when_loaded']})
            manifest['artifacts'][target['file']] = report
        for source, before in source_hashes.items():
            if hashlib.sha256((ROOT / source).read_bytes()).hexdigest() != before:
                raise ValueError('Build input changed during compilation: ' + source)
        (temp / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        output.mkdir(parents=True, exist_ok=True)
        for filename in [*manifest['artifacts'], 'manifest.json']:
            os.replace(temp / filename, output / filename)
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default=os.environ.get('ARM_CC'), help='ARM uClibc1.0.28 compiler')
    p.add_argument('--readelf', default=os.environ.get('READELF', 'readelf'))
    p.add_argument('--target', choices=(*TARGETS, 'all'), default='all')
    p.add_argument('--output-dir', type=Path, default=ROOT / 'output/sdk-trial')
    a = p.parse_args()
    try:
        if not a.cc or not shutil.which(a.cc):
            raise ValueError('Set ARM_CC or --cc to an available ARM uClibc1.0.28 compiler')
        if not shutil.which(a.readelf):
            raise ValueError('Host readelf tool is unavailable')
        manifest = build(a.cc, a.readelf, a.target, a.output_dir)
        print('Built experimental SDK-host trials in', a.output_dir)
        for name, artifact in manifest['artifacts'].items():
            print(name + ': sha256=' + artifact['sha256'])
        print('No camera access or target execution. These shared objects are not firmware updates.')
        return 0
    except (OSError, ValueError, KeyError, struct.error, UnicodeError) as error:
        print('Trial build failed: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
