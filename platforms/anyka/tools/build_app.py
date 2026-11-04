#!/usr/bin/env python3
"""Assemble an APP-only test image from explicit local inputs; never flashes.

The immutable stock ROOTFS/kernel and CONFIG partition are retained. This is an
interim SDK-host design: proprietary vendor code remains in a guarded binary,
but its original main exits125 and vendor startup daemons are not installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PARTITION_BYTES = 0x500000
SDK_HOST_SHA = '3507ad258eb74912bce1ae9dbc514cbe6c3e6460d2434dfdc2a85896b5f718ca'
MODULES = ('ak_rtc mmc_core mmc_block ak_mci ak_saradc ak_i2c ak_pcm ak_isp sensor_mis2006 '
           'ak_gpio_keys ak_ion ak_leds ak_uio ak_motor cfg80211 usb-common usbcore ak_hcd rtl8188ftv').split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def arm_elf(path, kind):
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ARM ELF32: ' + str(path))
    actual_kind, machine = struct.unpack_from('<HH', data, 16)
    flags = struct.unpack_from('<I', data, 36)[0]
    if actual_kind != kind or machine != 40 or flags & 0xff000000 != 0x05000000 or flags & 0x400:
        raise ValueError('Wrong ARM/EABI/soft-float artifact: ' + str(path))


def pad_partition(data):
    if len(data) < 96 or data[:4] != b'hsqs':
        raise ValueError('Not a SquashFS image')
    used = struct.unpack_from('<Q', data, 40)[0]
    if used < 96 or used > len(data) or used > PARTITION_BYTES:
        raise ValueError('APP SquashFS does not fit the exact5MiB partition')
    canonical = data[:used]
    return canonical, canonical + b'\xff' * (PARTITION_BYTES - used)


def pad_loop(data):
    # Linux loop devices expose whole sectors. A raw bytes_used-length file
    # may therefore appear shorter than its own SquashFS superblock declares.
    # Keep the compact RAM image block-aligned without changing NOR contents.
    canonical, _ = pad_partition(data)
    size = (len(canonical) + 4095) // 4096 * 4096
    return canonical + b'\xff' * (size - len(canonical))


def build(a):
    destination = a.output.resolve()
    if not destination.is_relative_to(ROOT / 'output'):
        raise ValueError('Output must remain under ignored output/')
    pins = json.loads((ROOT / 'config/vendor-inputs.json').read_text())
    vendor = a.vendor.resolve()
    manifest = json.loads((vendor / 'manifest.json').read_text())
    if manifest.get('board') != pins['board'] or manifest.get('inputs') != pins['images']:
        raise ValueError('Vendor import belongs to a different stock revision')
    if sha(a.sdk_host) != SDK_HOST_SHA:
        raise ValueError('SDK host hash mismatch; prepare the reviewed fail-closed host first')
    arm_elf(a.sdk_host, 2)
    arm_elf(a.application, 3)
    for file in (a.hostapd, a.hostapd_cli, a.installer, a.camera_login, a.camera_time):
        arm_elf(file, 2)
    selected = [('app/modules/' + name + '.ko', 'modules/' + name + '.ko') for name in MODULES]
    selected += [('app/bin/' + name, 'libexec/camera/' + name) for name in ('wpa_supplicant', 'wpa_cli')]
    selected += [('app/local/' + name, 'libexec/camera/' + name) for name in ('isp_mis2006_mipi_2lane_h3b.conf', 'ak_font_16.bin')]
    # Never stage on the project mount: FAT/network shares may reject symlinks.
    with tempfile.TemporaryDirectory(prefix='ptz-app-build-') as tmp:
        tmp = Path(tmp)
        stage = tmp / 'usr'
        shutil.copytree(ROOT / 'rootfs-overlay/usr', stage)
        for source, target in selected:
            file = vendor / source
            expected = manifest.get('assets', {}).get(source)
            if not expected or file.is_symlink() or not file.is_file() or file.stat().st_size != expected['bytes'] or sha(file) != expected['sha256']:
                raise ValueError('Missing/modified imported asset: ' + source)
            out = stage / target
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, out)
        inputs = {'sdk-host': a.sdk_host, 'camera-application.so': a.application,
                  'hostapd': a.hostapd, 'hostapd_cli': a.hostapd_cli, 'camera-installer': a.installer, 'camera-login': a.camera_login, 'camera-time': a.camera_time}
        for name, file in inputs.items():
            shutil.copyfile(file, stage / 'libexec/camera' / name)
        inventory = {}
        for file in sorted(stage.rglob('*')):
            if file.is_file():
                file.chmod(0o600 if file.name == 'bootstrap.auth' else 0o755 if file.name not in ('ak_font_16.bin', 'isp_mis2006_mipi_2lane_h3b.conf') else 0o644)
                inventory[str(file.relative_to(stage))] = {'bytes': file.stat().st_size, 'sha256': sha(file)}
        image = tmp / 'app.squashfs'
        command = [a.mksquashfs, str(stage), str(image), '-noappend', '-comp', 'xz',
                   '-b', '131072', '-all-root', '-no-xattrs', '-no-progress', '-processors', '1',
                   '-mkfs-time', str(a.epoch), '-all-time', str(a.epoch)]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        canonical, padded = pad_partition(image.read_bytes())
        loop = pad_loop(canonical)
        destination.mkdir(parents=True, exist_ok=True)
        (destination / 'app.squashfs').write_bytes(canonical)
        (destination / 'app.loop.bin').write_bytes(loop)
        (destination / 'app.partition.bin').write_bytes(padded)
    record = {'schema': 1, 'board': pins['board'], 'target': 'APP mtd7 only',
              'offset': 0x300000, 'partition_bytes': PARTITION_BYTES,
              'squashfs_bytes': len(canonical), 'compression': 'xz', 'block_bytes': 131072,
              'loop_bytes': len(loop), 'loop_alignment': 4096,
              'loop_sha256': hashlib.sha256(loop).hexdigest(),
              'app_sha256': hashlib.sha256(padded).hexdigest(),
              'app_blake2b': hashlib.blake2b(padded).hexdigest(), 'files': inventory,
              'physical_boot_tested': False, 'flash_ready': False,
              'architecture': 'interim guarded SDK host; original main blocked, proprietary code retained',
              'untested': ['cold boot', 'radio station/AP fallback', 'RAM headroom', 'normal APP unmount', 'rollback/recovery'],
              'build_epoch': a.epoch}
    (destination / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--vendor', type=Path, default=ROOT / 'base/vendor')
    p.add_argument('--sdk-host', type=Path, default=ROOT / 'base/sdk/sdk-host')
    p.add_argument('--application', type=Path, default=ROOT / 'output/application/camera-application.so')
    p.add_argument('--hostapd', type=Path, default=ROOT / 'output/network/hostapd')
    p.add_argument('--hostapd-cli', type=Path, default=ROOT / 'output/network/hostapd_cli')
    p.add_argument('--installer', type=Path, required=True, help='Board/trust-key-specific static installer')
    p.add_argument('--camera-login', type=Path, required=True, help='Own credential-aware ARM Telnet login helper')
    p.add_argument('--camera-time', type=Path, default=ROOT / 'output/application/camera-time', help='Own bounded SNTP client')
    p.add_argument('--output', type=Path, default=ROOT / 'output/app')
    p.add_argument('--mksquashfs', default='mksquashfs')
    p.add_argument('--epoch', type=int, default=0)
    a = p.parse_args()
    try:
        r = build(a)
        print('Created APP test image:', r['squashfs_bytes'], '/', PARTITION_BYTES, 'bytes; hardware validation required.')
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        p.exit(1, str(e) + '\n')


if __name__ == '__main__':
    main()
