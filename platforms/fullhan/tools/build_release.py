#!/usr/bin/env python3
"""Compile the same camera runtime for signed web OTA or bootloader SD flashing.

All operations are local files/compiler operations. Never accesses a camera,
mounts/formats a card, or writes flash.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def signing_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError('The web updater requires an Ed25519 PEM private key')
    public = key.public_key().public_bytes(serialization.Encoding.Raw,
                                          serialization.PublicFormat.Raw)
    if public != (ROOT / 'rootfs-overlay/usr/share/camera/update.pub').read_bytes():
        raise ValueError('Signing key does not match the public key baked into this firmware')
    return key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('web', 'sd', 'both'), required=True)
    parser.add_argument('--private-key', type=Path,
                        help='existing signing key; required for web/both, never copied to outputs')
    parser.add_argument('--sequence', type=int,
                        help='web anti-replay sequence: strictly above the camera accepted value')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'base/kernel-no-watchdog.uImage')
    parser.add_argument('--cc', help='ARMv6 soft-float compiler override')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'output/release')
    args = parser.parse_args()
    key = None
    if args.target in ('web', 'both'):
        if not args.private_key or args.sequence is None or not 1 < args.sequence < 2**64:
            parser.error('web/both requires --private-key and --sequence (2 through 2^64-1)')
        key = signing_key(args.private_key)
    elif args.private_key is not None or args.sequence is not None:
        parser.error('--private-key and --sequence apply only to web/both')
    # Pin the kernel we actually boot-tested, rather than accidentally choosing
    # the older stock driver with its autonomous watchdog timer.
    patch = json.loads((ROOT / 'config/kernel-patch.json').read_text())
    if sha(args.kernel.read_bytes()) != patch['kernel_uimage_sha256']:
        raise ValueError('Release kernel differs from the reviewed watchdog-disabled image')
    compiler = args.cc or os.environ.get('ARM_CC')
    build = [sys.executable, ROOT / 'tools/build.py', '--kernel', args.kernel]
    if compiler:
        build += ['--cc', compiler]
    run(*build)
    environment = os.environ.copy()
    if compiler:
        environment['ARM_CC'] = compiler
    subprocess.run([sys.executable, str(ROOT / 'tools/prepare_ram_root_firmware.py')],
                   check=True, env=environment)
    run(sys.executable, ROOT / 'tools/test_raminit.py', '--directory',
        ROOT / 'output/build/ram-root-firmware', '--runtime-manifest', ROOT / 'output/build/manifest.json')
    run(sys.executable, ROOT / 'tools/test_source_readonly.py')
    import firmware_package
    import sd_package
    runtime = json.loads((ROOT / 'output/build/manifest.json').read_text())
    images = {2: (ROOT / 'output/build/kernel.uImage').read_bytes(),
              3: (ROOT / 'output/build/ram-root-firmware/rootfs.squashfs').read_bytes()}
    version = json.loads((ROOT / 'config/build.json').read_text())['version']
    common = {'version': version, 'board': runtime['board'],
              'images': {str(kind): {'bytes': len(data), 'sha256': sha(data)}
                         for kind, data in images.items()},
              'protected_partitions': [0, 1, 2, 5],
              'physical_flash_tested': False,
              'nor_container_boot_tested': False,
              'configuration': 'Existing Wi-Fi, logins and camera settings preserved'}
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.release-', dir=args.output_dir) as temp:
        temp = Path(temp)
        if key is not None:
            # Import the existing envelope implementation without generating or
            # replacing any signing key or device trust anchor.
            import importlib.util
            spec = importlib.util.spec_from_file_location('camera_sign', ROOT / 'tools/sign_update.py')
            signer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(signer)
            payload = firmware_package.build(images)
            capsule = signer.envelope(payload, key, args.sequence)
            web = temp / 'web'
            web.mkdir()
            (web / 'camera-firmware.cap').write_bytes(capsule)
            report = dict(common, target='web', sequence=args.sequence,
                          capsule_sha256=sha(capsule), signed=True)
            (web / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
            (web / 'README.txt').write_text(
                'WEB UPDATE: upload camera-firmware.cap on the camera dashboard, port81.\n'
                'Review the verified candidate, then select Install & restart.\n'
                'Requires the custom firmware running from internal flash; RAM trials cannot install.\n'
                'Sequence must exceed the accepted value shown by the camera.\n'
                'A successful upload alone does not install firmware.\n')
            (web / 'SHA256SUMS').write_text(sha(capsule) + '  camera-firmware.cap\n')
        if args.target in ('sd', 'both'):
            sd = temp / 'sd'
            sd.mkdir()
            image = sd_package.build(images)
            details = sd_package.inspect(image)
            (sd / 'linux.bin').write_bytes(image)
            report = dict(common, target='sd', signed=False, package=details,
                          automatic_on_power_on=True)
            (sd / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
            (sd / 'SHA256SUMS').write_text(sha(image) + '  linux.bin\n')
            (sd / 'README.txt').write_text(
                'Firmware version: ' + version + '\n' +
                'AUTOMATIC FLASH INSTALLER - exact FH8626V100/MIS2006 8MiB camera only.\n'
                'This is actual flash installation, unlike the older SD preflight bundle.\n'
                '1. Back up the card; use one MBR/FAT32 partition.\n'
                '2. Copy linux.bin to the card ROOT, without an extra folder.\n'
                '   No custom.sh or other installer program is needed.\n'
                '3. Verify SHA256SUMS after copying; safely eject the card.\n'
                '4. Power OFF the camera, insert the card, then power ON.\n'
                '   The existing bootloader writes kernel/rootfs before Linux starts.\n'
                '5. Keep power connected throughout installation. For a new device,\n'
                '   monitor UART for errors and successful custom boot;\n'
                '   do not assume completion from an LED or a fixed waiting time.\n'
                '6. After the dashboard and live video/audio work, power off, remove\n'
                '   the card and delete/rename linux.bin on your computer before reuse.\n'
                '   Leaving it present rechecks/reinstalls this image on future boots.\n'
                'Web: http://CAMERA_IP:81 ; RTSP: rtsp://CAMERA_IP:8554/native\n'
                'Existing custom Wi-Fi/logins/settings are preserved. New defaults: admin / camera123.\n'
                'If no usable saved Wi-Fi exists, automatic recovery hotspot starts.\n'
                'Bootloader, boot environment and configuration partitions are not included.\n'
                'SD flashing uses bootloader CRC checks, not signed web anti-rollback.\n'
                'A power loss can leave a partial single-bank install; UART/SD recovery may be needed.\n'
                'See the project hardware-validation record for tested builds and remaining limits.\n')
            with zipfile.ZipFile(temp / 'camera-sd-installer.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
                for path in sorted(sd.iterdir()):
                    archive.write(path, path.name)
        # Publish only requested targets. Never carry a signing key into these
        # directories or touch mounted cards/device files.
        for name in ('web', 'sd'):
            source = temp / name
            if source.exists():
                destination = args.output_dir / name
                destination.mkdir(exist_ok=True)
                for path in source.iterdir():
                    os.replace(path, destination / path.name)
        if (temp / 'camera-sd-installer.zip').exists():
            os.replace(temp / 'camera-sd-installer.zip', args.output_dir / 'camera-sd-installer.zip')
            shutil.copyfile(args.output_dir / 'camera-sd-installer.zip',
                            args.output_dir / ('camera-sd-installer-' + version + '.zip'))
    print('Built requested target(s):', args.target, 'in', args.output_dir)
    print('Local build only. No card/device was modified; physical installation remains untested.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
