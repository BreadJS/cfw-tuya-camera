#!/usr/bin/env python3
"""Build signed Anyka APP-only web/SD candidates locally; never access a camera."""
import argparse
import hashlib
import json
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import tarfile
import zipfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'ak39ev330-mis2006-ptz-8m-v1'
APP_BYTES = 0x500000
HEADER_BYTES = 192
STOCK_BUNDLE = 'HT_IPC167KM_TUYA_AK3918EV330'
STOCK_BOOTSTRAP_VERSION = '3.10.57.0'


def build_stock_sd(app):
    """Exact original SD tar+1024B footer. MD5 is transport checking only.

    The two allowlisted regular members select only APP in stock update_tmp.sh.
    No environment/DTB/kernel/rootfs/CONFIG partition image is included.
    """
    if len(app) != APP_BYTES:
        raise ValueError('stock bootstrap requires exact APP partition image')
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        members = {'usr.sqsh4': app,
                   'usr.sqsh4.md5': (hashlib.md5(app, usedforsecurity=False).hexdigest()+'\n').encode()}
        for name, data in members.items():
            item = tarfile.TarInfo(name)
            item.size = len(data); item.mode = 0o644; item.uid = item.gid = item.mtime = 0
            archive.addfile(item, io.BytesIO(data))
    payload = raw.getvalue()
    metadata = ('bundle='+STOCK_BUNDLE+'\nversion='+STOCK_BOOTSTRAP_VERSION+
                '\nauthor=CamerHack\nmd5='+hashlib.md5(payload, usedforsecurity=False).hexdigest()+'\n').encode()
    if len(metadata) > 1024:
        raise ValueError('stock footer too large')
    return payload + metadata.ljust(1024, b'\n')


def build_signed_capsule(app, key, sequence):
    """Exact immutable APP payload; independent of SD/web transport."""
    if not 0 < sequence < 2**64:
        raise ValueError('sequence must be 1 through 2^64-1')
    if len(app) != APP_BYTES or app[:4] != b'hsqs' or app[28:30] != b'\x04\0':
        raise ValueError('APP must be exact 5MiB SquashFS4 partition image')
    used = struct.unpack_from('<Q', app, 40)[0]
    if not 96 <= used <= APP_BYTES or app[used:] != b'\xff' * (APP_BYTES-used):
        raise ValueError('SquashFS used length or ff padding invalid')
    header = (b'AKCAMO1\0' + struct.pack('<II', 1, HEADER_BYTES) +
              BOARD.encode().ljust(32, b'\0') + struct.pack('<QQ', sequence, APP_BYTES) +
              hashlib.blake2b(app).digest())
    assert len(header) == 128
    return header + key.sign(header) + app


def load_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError('existing Ed25519 private key required; no key is generated/replaced')
    return key, key.public_key().public_bytes(serialization.Encoding.Raw,
                                            serialization.PublicFormat.Raw)


def publish_installer(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.installer-', dir=destination.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(source.read_bytes())
        output.flush()
        os.fsync(output.fileno())
    try:
        temporary.chmod(0o755)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def trust_header(path, public, images):
    pins = json.loads((ROOT / 'config/vendor-inputs.json').read_text())
    trust = json.loads((ROOT / 'config/update-trust.json').read_text())
    if trust.get('schema') != 1 or trust.get('board_id') != pins['board']:
        raise ValueError('immutable update trust profile differs from board')
    digests = []
    for name in ('dtb', 'kernel', 'rootfs'):
        pin = pins['images'][name]
        expected = trust['immutable'][name]
        if any(expected.get(field) != pin[field] for field in ('bytes', 'sha256')):
            raise ValueError('immutable hash profile/input pin mismatch: ' + name)
        if images[name] is not None:
            data = images[name].read_bytes()
            if (len(data) != pin['bytes'] or hashlib.sha256(data).hexdigest() != pin['sha256'] or
                    hashlib.blake2b(data).hexdigest() != expected['blake2b']):
                raise ValueError(name + ' input differs from the reviewed board revision')
        else:
            imported = json.loads((ROOT/'base/vendor/manifest.json').read_text())
            if imported.get('board') != pins['board'] or imported.get('inputs') != pins['images']:
                raise ValueError('reviewed vendor import manifest required without raw ' + name)
        digest = bytes.fromhex(expected['blake2b'])
        if len(digest) != 64:
            raise ValueError('invalid immutable digest length')
        digests.append(digest)
    def array(data):
        return '{' + ','.join('0x%02x' % b for b in data) + '}'
    path.write_text('/* Generated trust anchor; contains no private key. */\n'
                    'static const unsigned char anyka_update_public_key[32]=' + array(public) + ';\n'
                    'static const unsigned char anyka_update_immutable_digest[3][64]={' +
                    ','.join(array(d) for d in digests) + '};\n')


def compile_installer(cc, directory, public, images):
    trust_header(directory / 'update_trust.h', public, images)
    source = ROOT / 'src/update'
    output = directory / 'camera-app-installer'
    command = [cc, '-std=c11', '-Os', '-static', '-march=armv5te', '-mfloat-abi=soft',
               '-Wall', '-Wextra', '-Werror', '-I' + str(directory),
               str(source/'install_main.c'), str(source/'capsule.c'),
               str(source/'receipt.c'),
               str(source/'vendor/monocypher.c'), str(source/'vendor/monocypher-ed25519.c'),
               '-o', str(output)]
    subprocess.run(command, check=True)
    # A dynamic helper would retain overwritten APP dependencies. Require the
    # ELF32 ARM EABI executable to have no PT_INTERP or PT_DYNAMIC segment.
    data = output.read_bytes()
    if data[:5] != b'\x7fELF\x01' or struct.unpack_from('<H', data, 18)[0] != 40:
        raise ValueError('installer compiler did not produce ELF32 ARM')
    phoff = struct.unpack_from('<I', data, 28)[0]
    entsize, count = struct.unpack_from('<HH', data, 42)
    if any(struct.unpack_from('<I', data, phoff+i*entsize)[0] in (2, 3) for i in range(count)):
        raise ValueError('installer is dynamically linked')
    return output


def checked_boot_evidence(path, app):
    if path is None:
        return None
    evidence = json.loads(path.read_text())
    digest = hashlib.blake2b(app).hexdigest()
    required = ('media', 'web', 'network')
    boot_id = evidence.get('boot_id')
    try:
        valid_boot_id = isinstance(boot_id,str) and str(uuid.UUID(boot_id)) == boot_id
    except (ValueError,AttributeError):
        valid_boot_id = False
    ram_app = (evidence.get('boot_mode') == 'ram_app_overlay' and
               evidence.get('app_image_started') is True and evidence.get('rootfs_pinned') is True)
    nor_app = evidence.get('app_root_boot') is True and evidence.get('boot_mode') != 'ram_app_overlay'
    if (evidence.get('schema') != 1 or evidence.get('board_id') != BOARD or
            evidence.get('payload_blake2b') != digest or
            not valid_boot_id or
            any(evidence.get(field) is not True for field in required) or not (ram_app or nor_app)):
        raise ValueError('boot evidence must match exact APP and prove media/web/network plus a real RAM APP overlay or NOR APP boot')
    return evidence


def check_app_runtime(image, installer):
    """Do not sign an image whose embedded trust anchor or SDK guard differs."""
    sdk = json.loads((ROOT/'config/sdk-host.json').read_text())
    expected = [('libexec/camera/camera-installer', installer.stat().st_size,
                 hashlib.sha256(installer.read_bytes()).hexdigest()),
                ('libexec/camera/sdk-host', sdk['source_bytes'], sdk['sdk_host_sha256'])]
    for member, size, digest in expected:
        process = subprocess.Popen(['unsquashfs','-cat',str(image),member],
                                   stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
        try:
            data = process.stdout.read(size+1)
            if len(data) > size:
                process.kill()
                raise ValueError('oversized embedded runtime member: ' + member)
            if process.wait(timeout=30) or len(data) != size or hashlib.sha256(data).hexdigest() != digest:
                raise ValueError('APP SDK guard/trust anchor mismatch: ' + member)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            process.stdout.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=('web', 'sd', 'both'), required=True)
    p.add_argument('--check', action='store_true')
    p.add_argument('--candidate-only', action='store_true', help='explicit unbootvalidated RAM candidate; never flash-ready')
    p.add_argument('--installer-only', action='store_true', help='compile trusted static helper before APP image construction')
    p.add_argument('--app-image', type=Path, help='package exact already-built image; otherwise compile application and APP from source')
    p.add_argument('--private-key', type=Path, default=Path(os.environ.get('CAMERA_SIGNING_KEY',
                   str(Path(os.environ.get('XDG_DATA_HOME',str(Path.home()/'.local/share')))/'camerhack/ptz-firmware-signing.pem'))))
    p.add_argument('--sequence', type=int)
    p.add_argument('--cc', default=os.environ.get('ARM_CC'))
    p.add_argument('--dtb', type=Path, default=ROOT/'base/vendor/dtb.bin')
    p.add_argument('--kernel', type=Path, default=ROOT/'base/vendor/kernel.bin')
    p.add_argument('--rootfs', type=Path, help='optional original raw image; default uses reviewed public hash profile plus validated vendor import manifest')
    p.add_argument('--hostapd-source', type=Path)
    p.add_argument('--libnl-source', type=Path)
    p.add_argument('--boot-evidence', type=Path)
    p.add_argument('--output-dir', type=Path, default=ROOT/'output/release')
    a = p.parse_args()
    missing = []
    required = ['private_key', 'dtb', 'kernel']
    if a.app_image is not None:
        required.append('app_image')
    if a.rootfs is not None:
        required.append('rootfs')
    for name in required:
        value = getattr(a, name)
        if value is None or not value.is_file():
            missing.append(name.replace('_', '-'))
    if not a.cc or not shutil.which(a.cc):
        missing.append('ARM static compiler (--cc/ARM_CC)')
    if not a.installer_only and (a.sequence is None or not 0 < a.sequence < 2**64):
        missing.append('positive --sequence')
    if missing:
        print('No package generated; missing actual artifacts: ' + ', '.join(missing))
        return 2
    key, public = load_key(a.private_key)
    images = {name: getattr(a, name) for name in ('dtb', 'kernel', 'rootfs')}
    if a.installer_only:
        if a.check:
            with tempfile.TemporaryDirectory(prefix='ptz-installer-check-') as td:
                trust_header(Path(td)/'update_trust.h',public,images)
            print('Installer trust inputs passed; no artifact compiled.')
            return 0
        a.output_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.installer-', dir=a.output_dir) as td:
            installer = compile_installer(a.cc, Path(td), public, images)
            publish_installer(installer, a.output_dir/'camera-app-installer')
        print('Built trusted static installer only:', a.output_dir/'camera-app-installer')
        return 0
    if a.app_image is None:
        if a.check:
            a.app_image = ROOT/'output/app/app.partition.bin'
            if not a.app_image.is_file():
                print('No package generated: build APP source image first for --check.')
                return 2
        else:
            with tempfile.TemporaryDirectory(prefix='anyka-release-installer-') as td:
                installer = compile_installer(a.cc, Path(td), public, images)
                destination = ROOT/'output/update/camera-app-installer'
                publish_installer(installer, destination)
            if a.hostapd_source is not None or a.libnl_source is not None:
                if a.hostapd_source is None or a.libnl_source is None:
                    raise ValueError('supply both network source paths')
                subprocess.run([sys.executable,str(ROOT/'tools/build_network.py'),'--cc',a.cc,
                                '--hostapd-source',str(a.hostapd_source),
                                '--libnl-source',str(a.libnl_source)],check=True)
            subprocess.run([sys.executable,str(ROOT/'tools/build_application.py'),
                            '--cc',a.cc,'--enable-osd'],check=True)
            subprocess.run([sys.executable,str(ROOT/'tools/build_app.py'),
                            '--installer',str(destination),
                            '--camera-login',str(ROOT/'output/application/camera-login')],check=True)
            a.app_image = ROOT/'output/app/app.partition.bin'
    app = a.app_image.read_bytes()
    capsule = build_signed_capsule(app, key, a.sequence)
    evidence = checked_boot_evidence(a.boot_evidence, app)
    if evidence is None and not a.candidate_only:
        print('No flash-ready package generated: matching actual APP --boot-evidence required; --candidate-only is explicit RAM review mode.')
        return 2
    if a.target in ('sd', 'both') and (a.candidate_only or evidence is None):
        print('No automatic stock SD package generated: matching exact APP boot evidence required.')
        return 2
    images = {name: getattr(a, name) for name in ('dtb', 'kernel', 'rootfs')}
    # Validate immutable inputs even for --check. No compiler or package writes.
    if a.check:
        with tempfile.TemporaryDirectory(prefix='ptz-release-check-') as td:
            trust_header(Path(td)/'update_trust.h', public, images)
        print('Offline input gates passed; physical install and post-flash boot remain unverified.')
        return 0
    a.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.anyka-release-', dir=a.output_dir) as td:
        stage = Path(td)
        installer = compile_installer(a.cc, stage, public, images)
        check_app_runtime(a.app_image, installer)
        manifest = {'schema': 1, 'board_id': BOARD, 'sequence': a.sequence,
                    'payload_blake2b': hashlib.blake2b(app).hexdigest(),
                    'capsule_sha256': hashlib.sha256(capsule).hexdigest(),
                    'app_bytes': len(app), 'app_offset': 0x300000, 'app_mtd': 7,
                    'protected_mtd': list(range(7)), 'signed': True,
                    'candidate_only': a.candidate_only, 'flash_ready': not a.candidate_only and evidence is not None,
                    'physical_install_verified': False, 'post_flash_boot_confirmed': False,
                    'single_bank_power_loss_rollback': False,
                    'boot_evidence': evidence,
                    'public_key_hex': public.hex(),
                    'installer_sha256': hashlib.sha256(installer.read_bytes()).hexdigest()}
        for target in ('web', 'sd'):
            if a.target not in (target, 'both'):
                continue
            directory = stage/target
            directory.mkdir()
            if target == 'web':
                (directory/'camera-app.cap').write_bytes(capsule)
                shutil.copyfile(installer, directory/'camera-app-installer')
                (directory/'camera-app-installer').chmod(0o755)
            if target == 'sd':
                stock = build_stock_sd(app)
                (directory/'firmware.bin').write_bytes(stock)
                (directory/'camera-app.cap').write_bytes(capsule)
                manifest = dict(manifest, stock_bootstrap_version=STOCK_BOOTSTRAP_VERSION,
                                stock_bundle=STOCK_BUNDLE, stock_package_sha256=hashlib.sha256(stock).hexdigest(),
                                stock_authentication='MD5 transport checks only; no Ed25519 verification by stock updater',
                                stock_config_cleanup='/etc/config/isp_*.conf removed by original script; own APP profile included',
                                signed=False, signed_capsule_included=True, stock_bootstrap_signed=False,
                                stock_installer_readback=False, custom_installer_readback_enforced=True)
            (directory/'manifest.json').write_text(json.dumps(dict(manifest, target=target), indent=2)+'\n')
            (directory/'README.txt').write_text(
                ('STOCK SD BOOTSTRAP: copy firmware.bin to FAT32 card root; remove other .bin/update.tar files.\n'
                 'Power OFF, insert card, power ON. Stock updater installs APP and reboots.\n'
                 'MD5 transport checks only; no Ed25519 verification/readback by original updater.\n'
                 'Stock updater removes /etc/config/isp_*.conf; own APP contains its sensor profile.\n'
                 'After confirmed web/video/audio boot, remove firmware.bin from card.\n' if target == 'sd' else
                 'WEB: upload camera-app.cap to custom firmware port81, review, then Install.\n') +
                'Anyka APP-only update. Exact AK39EV330/MIS2006 PTZ board only.\n'
                'Protected bootloader/environment/DTB/kernel/rootfs/config partitions are never erased.\n'
                'Installer changes only APP mtd7 and the custom update journal in CONFIG.\n'
                'Keep power connected throughout installation. Single-bank flash has no power-loss rollback.\n'
                'Signature/readback success is not post-flash boot confirmation.\n'
                'See docs/UPDATE.md for trusted staging/quiesce and boot-health contracts.\n')
            (directory/'SHA256SUMS').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.name+'\n'
                                                     for f in sorted(directory.iterdir()) if f.is_file()))
            archive = stage/('camera-'+target+('-RAM-CANDIDATE' if a.candidate_only else '-update')+'.zip')
            with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
                for f in sorted(directory.iterdir()):
                    z.write(f, f.name)
            os.replace(archive, a.output_dir/archive.name)
            destination = a.output_dir/target
            if destination.exists():
                shutil.rmtree(destination)
            os.replace(directory, destination)
    print('Built signed APP-only candidate(s):', a.target)
    print('No camera accessed or flash written. Physical installation remains pending.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
