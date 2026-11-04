#!/usr/bin/env python3
"""Offline cross-build of pinned hostapd sources; never runs target binaries.

Obtain the archives/checkouts named in config/network-sources.json separately.
Only the reviewed content is accepted. libnl-tiny is linked statically; target
runtime only needs immutable ROOTFS libc.so.0. No source download is implicit.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from build_trial import inspect_toolchain, ROOT


def tree_hash(directory):
    h = hashlib.sha256()
    for file in sorted(directory.rglob('*')):
        relative = file.relative_to(directory)
        if '.git' in relative.parts:
            continue
        if file.is_symlink():
            raise ValueError('Unexpected source symlink: ' + str(relative))
        if file.is_file():
            h.update(str(relative).encode() + b'\0' + file.read_bytes() + b'\0')
    return h.hexdigest()


def command(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default=os.environ.get('ARM_CC'))
    p.add_argument('--hostapd-source', type=Path, required=True, help='Original .tar.gz')
    p.add_argument('--libnl-source', type=Path, required=True, help='Clean pinned git checkout')
    p.add_argument('--output', type=Path, default=ROOT / 'output/network')
    a = p.parse_args()
    try:
        if not a.cc:
            raise ValueError('Set ARM_CC or --cc to the reviewed ARM/uClibc1.0.28 compiler')
        cc = shutil.which(a.cc)
        if not cc:
            raise ValueError('Compiler not found')
        compiler = inspect_toolchain(cc)
        pin = json.loads((ROOT / 'config/network-sources.json').read_text())
        if hashlib.sha256(a.hostapd_source.read_bytes()).hexdigest() != pin['hostapd']['sha256']:
            raise ValueError('hostapd archive hash mismatch')
        if tree_hash(a.libnl_source) != pin['libnl_tiny']['tree_sha256']:
            raise ValueError('libnl-tiny source tree hash mismatch (use a clean pinned checkout)')
        a.output = a.output.resolve()
        if not a.output.is_relative_to(ROOT / 'output'):
            raise ValueError('Build output must stay under ignored output/')
        a.output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='ptz-network-') as tmp:
            tmp = Path(tmp)
            with tarfile.open(a.hostapd_source) as archive:
                archive.extractall(tmp, filter='data')
            shutil.copytree(a.libnl_source, tmp / 'libnl', ignore=shutil.ignore_patterns('.git'))
            log = open(a.output / 'build.log', 'w')
            common = dict(stdout=log, stderr=subprocess.STDOUT)
            command(['cmake', '-S', tmp / 'libnl', '-B', tmp / 'nl-build',
                     '-DCMAKE_C_COMPILER=' + cc,
                     '-DCMAKE_C_FLAGS=-Os -mcpu=arm926ej-s -mfloat-abi=soft'], **common)
            command(['cmake', '--build', tmp / 'nl-build', '--target', 'nl-tiny-static', '-j4'], **common)
            project = tmp / 'hostapd-2.12/hostapd'
            (project / '.config').write_text('''CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_TLS=internal
CONFIG_CRYPTO=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_NO_RADIUS=y
CONFIG_NO_ACCOUNTING=y
CONFIG_NO_VLAN=y
CONFIG_CTRL_IFACE=y
''' + f'CFLAGS += -Os -mcpu=arm926ej-s -mfloat-abi=soft -I{tmp}/libnl/include\n'
                + f'LDFLAGS += -L{tmp}/nl-build\n')
            command(['make', '-j4', 'CC=' + cc, 'PKG_CONFIG=true'], cwd=project, **common)
            prefix = cc[:-3] if cc.endswith('gcc') else ''
            if not prefix:
                raise ValueError('Compiler filename must end gcc to locate matching strip/readelf')
            files = {}
            for name in ('hostapd', 'hostapd_cli'):
                output = a.output / name
                command([prefix + 'strip', '-o', output, project / name], **common)
                dynamic = subprocess.check_output([prefix + 'readelf', '-d', output], text=True)
                needed = [line.split('[')[1].split(']')[0] for line in dynamic.splitlines() if '(NEEDED)' in line]
                if needed != ['libc.so.0']:
                    raise ValueError('Unexpected target dependency: ' + str(needed))
                output.chmod(0o755)
                files[name] = {'bytes': output.stat().st_size, 'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
            log.close()
        manifest = {'schema': 1, 'compiler': compiler, 'sources': pin, 'files': files,
                    'hardware_tested': False, 'firmware': False,
                    'license': 'hostapd BSD-3-Clause; libnl-tiny LGPL-2.1-or-later, retain source and notices'}
        (a.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print('Built ARM/uClibc AP tools; radio/AP behavior still requires hardware validation.')
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        p.exit(1, str(e) + '\n')


if __name__ == '__main__':
    main()
