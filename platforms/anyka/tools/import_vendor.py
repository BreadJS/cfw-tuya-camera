#!/usr/bin/env python3
"""Import allowlisted runtime assets from exact local stock images; no camera I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for image in ('rootfs', 'app', 'kernel', 'dtb'):
        p.add_argument('--' + image, required=True, type=Path)
    a = p.parse_args()
    pins = json.loads((ROOT / 'config/vendor-inputs.json').read_text())
    for name, expected in pins['images'].items():
        path = getattr(a, name)
        if path.stat().st_size != expected['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest() != expected['sha256']:
            p.error(name + ' does not match the validated stock revision; add a reviewed board profile first.')
    destination = ROOT / 'base/vendor'
    if destination.exists():
        p.error('base/vendor already exists; refusing to overwrite imported assets.')
    with tempfile.TemporaryDirectory(prefix='ptz-vendor-import-') as temp:
        temp = Path(temp)
        for name in ('rootfs', 'app'):
            subprocess.run(['unsquashfs', '-no-progress', '-d', str(temp / name),
                            str(getattr(a, name).resolve())], check=True, stdout=subprocess.DEVNULL)
        stage = temp / 'vendor'
        selected = []
        selected.append(('rootfs/bin/busybox', 'rootfs/bin/busybox'))
        for path in sorted((temp / 'rootfs/lib').glob('*.so*')):
            # Copy real runtime libraries only. No accounts, config, startup
            # scripts, Tuya applications, UUIDs or complete original images.
            if not path.is_symlink() and path.is_file():
                rel = str(path.relative_to(temp))
                selected.append((rel, rel))
        for path in sorted((temp / 'app/modules').glob('*.ko')):
            rel = str(path.relative_to(temp))
            selected.append((rel, rel))
        for name in ('wpa_supplicant', 'wpa_cli', 'iwconfig', 'iwlist', 'iwpriv', 'iwgetid'):
            selected.append(('app/bin/' + name, 'app/bin/' + name))
        selected.append(('app/local/isp_mis2006_mipi_2lane_h3b.conf',
                         'app/local/isp_mis2006_mipi_2lane_h3b.conf'))
        selected.append(('app/local/ak_font_16.bin', 'app/local/ak_font_16.bin'))
        assets = {}
        for source, target in selected:
            src = temp / source
            if not src.is_file() or src.is_symlink():
                raise ValueError('Missing allowlisted real file: ' + source)
            out = stage / target
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, out)
            assets[target] = {'bytes': out.stat().st_size,
                             'sha256': hashlib.sha256(out.read_bytes()).hexdigest()}
        for name in ('kernel', 'dtb'):
            out = stage / (name + '.bin')
            shutil.copyfile(getattr(a, name), out)
            assets[out.name] = pins['images'][name]
        manifest = {'board': pins['board'], 'inputs': pins['images'], 'assets': assets,
                    'media_sdk_included': False, 'hostapd_included': False,
                    'bootable_image': False}
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        shutil.copytree(stage, destination)
    print('Imported', len(assets), 'allowlisted assets into', destination)
    print('No SDK media library or working firmware image was created.')


if __name__ == '__main__':
    main()
