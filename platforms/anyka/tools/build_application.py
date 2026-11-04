#!/usr/bin/env python3
"""Build the owned Anyka SDK-host application; never deploy or flash a camera."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tempfile

from build_trial import ALLOWED_LIBRARIES, ARM_FLAGS, FLAGS, inspect_shared, inspect_toolchain, run

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    'src/anyka/application.c', 'src/anyka/application_runtime.c',
    'src/anyka/video_mode.c', 'src/anyka/media_backend.c', 'src/anyka/sensor_timing.c', 'src/anyka/audio_backend.c', 'src/anyka/sdk_api.c',
    'src/anyka/trial_guard.c', 'src/anyka/advanced_picture.c', 'src/anyka/management.c', 'src/anyka/board_control.c', 'src/anyka/board_settings.c',
    'src/anyka/encoder_control.c', 'src/anyka/picture_executor.c', 'src/anyka/reset_button.c', 'src/anyka/wifi.c', 'src/anyka/update_http.c',
    'src/native/rtsp/camera_rtsp.c', 'src/native/rtsp/h264_parameters.c', 'src/native/control/http.c',
    'src/native/control/credentials.c', 'src/display/settings.c',
    'src/preview/preview.c', 'src/preview/mp4.c', 'src/update/capsule.c',
    'src/update/receipt.c', 'src/update/vendor/monocypher.c',
    'src/update/vendor/monocypher-ed25519.c',
]


def inspect_executable(data, attributes, dynamic):
    """Validate the emitted helper, including its actual PT_INTERP segment."""
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Helper is not ELF32 little-endian')
    kind, machine, version, entry, phoff = struct.unpack_from('<HHIII', data, 16)
    flags, ehsize, phsize, phnum = struct.unpack_from('<IHHH', data, 36)
    if (kind, machine, version) != (2, 40, 1) or not entry:
        raise ValueError('Helper is not an ARM executable with an entry point')
    if flags & 0xff000000 != 0x05000000 or flags & 0x600 != 0x200:
        raise ValueError('Helper is not EABI5 soft-float')
    if ehsize != 52 or phsize != 32 or not 1 <= phnum <= 32 or phoff < 52 or phoff + phnum * phsize > len(data):
        raise ValueError('Invalid helper program table')
    interpreter = None
    executable_entry = False
    for index in range(phnum):
        kind, offset, address, _, filesz, memsz, permissions, _ = struct.unpack_from('<8I', data, phoff + index * phsize)
        if offset + filesz > len(data) or (kind == 1 and filesz > memsz):
            raise ValueError('Invalid helper segment bounds')
        if kind == 3:
            raw = data[offset:offset + filesz]
            if interpreter is not None or raw not in (b'/lib/ld-uClibc.so.0\0', b'/lib/ld-uClibc.so.1\0'):
                raise ValueError('Unexpected helper interpreter')
            interpreter = raw[:-1].decode('ascii')
        if kind == 1 and permissions & 1 and address <= entry < address + filesz:
            executable_entry = True
    cpu = re.search(r'Tag_CPU_arch:\s*(\S+)', attributes)
    if not interpreter or not executable_entry or not cpu or cpu[1] not in ('v5TE', 'v5TEJ'):
        raise ValueError('Helper is not ARMv5TE with a mapped executable entry/interpreter')
    if 'Tag_THUMB_ISA_use: Thumb-2' in attributes or '(RPATH)' in dynamic or '(RUNPATH)' in dynamic:
        raise ValueError('Unexpected helper architecture or runtime search path')
    needed = re.findall(r'Shared library: \[([^\]]+)\]', dynamic)
    if 'libc.so.0' not in needed or set(needed) - ALLOWED_LIBRARIES:
        raise ValueError('Unexpected helper runtime dependencies')
    return {'machine': 'ARM', 'cpu_arch': cpu[1], 'abi': 'EABI5-soft',
            'elf': 'ELF32-little-ET_EXEC', 'interpreter': interpreter,
            'needed': sorted(needed)}


def build(cc, readelf, output, enable_osd=False):
    output = output.resolve()
    if not output.is_relative_to((ROOT / 'output').resolve()):
        raise ValueError('Application output must remain under project output/')
    compiler = inspect_toolchain(cc)
    config = json.loads((ROOT / 'config/build.json').read_text())
    version = config['version']
    epoch = config['epoch']
    if type(epoch) is not int or not 1577836800 <= epoch <= 2147483647:
        raise ValueError('Image build epoch must be a representable UTC timestamp from2020 through2038')
    if not isinstance(version, str) or not version or len(version) > 63 or any(
            ord(c) < 32 or ord(c) > 126 for c in version):
        raise ValueError('Invalid application version')
    inputs = set(SOURCES)
    inputs.update(('src/telnet/login.c', 'src/time/camera_time.c'))
    for base in ('src/anyka', 'src/native/control', 'src/native/rtsp', 'src/display',
                 'src/preview', 'src/update'):
        inputs.update(str(p.relative_to(ROOT)) for p in (ROOT / base).rglob('*.h')
                      if p.name != 'ui.h')
    inputs.update(('src/native/control/ui.html', 'src/native/control/picture_controls.js',
                   'src/native/control/generate_ui.py', 'src/native/control/picture_common.js', 'src/board/camera_board_config.h', 'config/board-public.json', 'config/build.json',
                   'config/sdk-host.json', 'tools/build_application.py',
                   'tools/build_trial.py', 'tools/prepare_sdk_host.py'))
    hashes = {source: hashlib.sha256((ROOT / source).read_bytes()).hexdigest()
              for source in sorted(inputs)}
    run([sys.executable, ROOT / 'src/native/control/generate_ui.py'])
    defines = ['-DCAMERA_ANYKA_APPLICATION_CONSTRUCTOR',
               '-DCAMERA_APPLICATION_VERSION=' + json.dumps(version)]
    if enable_osd:
        defines.append('-DCAMERA_ANYKA_ENABLE_OSD')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.application-build-', dir=output.parent) as directory:
        temporary = Path(directory)
        artifact = temporary / 'camera-application.so'
        command = [cc, *FLAGS, *defines, '-Wl,-z,defs',
                   *[str(ROOT / source) for source in SOURCES],
                   '-ldl', '-lrt', '-o', str(artifact)]
        run(command)
        data = artifact.read_bytes()
        report = inspect_shared(data, run([readelf, '-A', artifact]))
        login = temporary / 'camera-login'
        run([cc, '-std=c11', '-Os', '-pthread', '-Wall', '-Wextra', '-Werror',
             *ARM_FLAGS, '-Wl,--hash-style=sysv', '-Wl,--build-id=none',
             ROOT / 'src/telnet/login.c', ROOT / 'src/native/control/credentials.c',
             '-o', login])
        login_data = login.read_bytes()
        login_report = inspect_executable(login_data, run([readelf, '-A', login]),
                                          run([readelf, '-d', login]))
        clock = temporary / 'camera-time'
        run([cc, '-std=c11', '-Os', '-Wall', '-Wextra', '-Werror', *ARM_FLAGS,
             '-Wl,--hash-style=sysv', '-Wl,--build-id=none',
             '-DCAMERA_IMAGE_BUILD_EPOCH=' + str(epoch), ROOT / 'src/time/camera_time.c', '-lrt', '-o', clock])
        clock_data = clock.read_bytes()
        clock_report = inspect_executable(clock_data, run([readelf, '-A', clock]),
                                          run([readelf, '-d', clock]))
        symbols = run([readelf, '-Ws', artifact])
        if 'camera_anyka_application' not in symbols:
            raise ValueError('Application entry symbol missing')
        if 'camera_anyka_rtsp_trial' in symbols:
            raise ValueError('Experimental trial entry was accidentally linked')
        for source, expected in hashes.items():
            if hashlib.sha256((ROOT / source).read_bytes()).hexdigest() != expected:
                raise ValueError('Source changed during build: ' + source)
        host = json.loads((ROOT / 'config/sdk-host.json').read_text())
        manifest = {
            'schema': 1, 'board': config['board'], 'version': version,
            'artifact': artifact.name, 'sha256': hashlib.sha256(data).hexdigest(),
            'bytes': len(data), 'compiler': compiler, 'elf': report,
            'login': {'artifact': login.name, 'sha256': hashlib.sha256(login_data).hexdigest(),
                      'bytes': len(login_data), 'credential_role': 'web', 'elf': login_report},
            'clock': {'artifact': clock.name, 'sha256': hashlib.sha256(clock_data).hexdigest(),
                      'bytes': len(clock_data), 'elf': clock_report, 'sets_clock': 'UTC via validated SNTP',
                      'image_build_epoch': epoch, 'build_floor_accuracy': 'unsynchronized'},
            'sources': hashes, 'flags': [*FLAGS, *defines, '-Wl,-z,defs'],
            'sdk_host_required': True, 'sdk_host_included': False,
            'required_sdk_host_sha256': host['sdk_host_sha256'],
            'osd_pool_compiled': bool(enable_osd),
            'osd_runtime_opt_in': 'CAMERA_OSD_ENABLE=1',
            'hardware_access_when_loaded': True, 'network_service_when_loaded': True,
            'camera_contacted_by_build': False, 'target_executed_by_build': False,
            'flash_modified': False, 'flashable': False,
            'limitations': ['Shared application only; boot/image/installer integration must be validated separately.',
                            'Requires reviewed private vendor SDK host; this artifact is not standalone firmware.'],
        }
        (temporary / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        output.mkdir(parents=True, exist_ok=True)
        os.replace(artifact, output / artifact.name)
        os.replace(login, output / login.name)
        os.replace(clock, output / clock.name)
        os.replace(temporary / 'manifest.json', output / 'manifest.json')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('ARM_CC'))
    parser.add_argument('--readelf', default=os.environ.get('READELF', 'readelf'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'output/application')
    parser.add_argument('--enable-osd', action='store_true',
                        help='Reserve reviewed OSD DMA pool; runtime additionally requires explicit enable')
    args = parser.parse_args()
    try:
        if not args.cc or not shutil.which(args.cc):
            raise ValueError('Set ARM_CC or --cc to the matching ARM uClibc1.0.28 compiler')
        if not shutil.which(args.readelf):
            raise ValueError('readelf is unavailable')
        result = build(args.cc, args.readelf, args.output_dir, args.enable_osd)
        print('Built', args.output_dir / result['artifact'])
        print('sha256=' + result['sha256'])
        print('Application only; no camera access, deployment or flash writes.')
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('Application build failed: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
