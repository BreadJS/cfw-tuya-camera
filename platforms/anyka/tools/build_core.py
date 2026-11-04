#!/usr/bin/env python3
"""Build the portable camera core. This does not build a flash image."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', action='store_true', help='build for development computer')
    p.add_argument('--cc', help='ARMv5 compiler, or host compiler with --host')
    p.add_argument('--ar', help='matching archiver')
    p.add_argument('--output-dir', type=Path)
    a = p.parse_args()
    cc = a.cc or os.environ.get('CC' if a.host else 'ARM_CC')
    cc = cc or ('cc' if a.host else shutil.which('arm-linux-gnueabi-gcc'))
    if not cc:
        p.error('Set ARM_CC to an ARMv5TE soft-float compiler; use --host for development.')
    if not shutil.which(cc):
        p.error('Compiler not found: ' + cc)
    ar = a.ar or os.environ.get('AR' if a.host else 'ARM_AR')
    if not ar:
        ar = subprocess.check_output([cc, '-print-prog-name=ar'], text=True).strip()
    if not shutil.which(ar):
        p.error('Archiver not found: ' + ar)
    out = a.output_dir or ROOT / 'output' / ('host' if a.host else 'armv5')
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(ROOT / 'src/native/control/generate_ui.py')], check=True)
    flags = ['-std=c11', '-Os', '-pthread', '-Wall', '-Wextra', '-Werror']
    if not a.host:
        flags += ['-mcpu=arm926ej-s', '-marm', '-mfloat-abi=soft']
    objects = []
    sources = json.loads((ROOT / 'config/sources.json').read_text())
    for source in sources:
        # The time service has its own entry point, rather than an archive member.
        if source == 'src/time/camera_time.c':
            continue
        obj = out / (source.replace('/', '_') + '.o')
        subprocess.run([cc, *flags, '-c', str(ROOT / source), '-o', str(obj)], check=True)
        if not a.host:
            attrs = subprocess.check_output(['readelf', '-A', str(obj)], text=True)
            if 'Tag_CPU_arch: v5TE' not in attrs:
                raise RuntimeError('Object is not marked ARMv5TE: ' + source)
        objects.append(obj)
    archive = out / 'libcamera-core.a'
    # Remove old members when the source inventory changes.
    archive.unlink(missing_ok=True)
    subprocess.run([ar, 'rcs', str(archive), *map(str, objects)], check=True)
    report = {'target': 'host' if a.host else 'armv5', 'artifact': archive.name,
              'camera_service': False, 'flashable': False, 'sources': sources,
              'backend': 'Anyka implementation still required'}
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Built portable core:', archive)
    print('This archive is not a camera application or an SD/web firmware update.')


if __name__ == '__main__':
    main()
