"""FH8626 static media service and kernel/RAM-root image module."""
import os
import subprocess
import sys

def application(work,cc,options):
    subprocess.run([sys.executable,str(work/'tools/build.py'),'--cc',str(cc),'--application-only'],check=True)
    return work/'output/build'

def image(work,cc,options):
    subprocess.run([sys.executable,str(work/'tools/build.py'),'--cc',str(cc)],check=True)
    environment=dict(os.environ,ARM_CC=str(cc))
    subprocess.run([sys.executable,str(work/'tools/prepare_ram_root_firmware.py')],check=True,env=environment)
    subprocess.run([sys.executable,str(work/'tools/test_raminit.py'),'--directory',str(work/'output/build/ram-root-firmware'),'--runtime-manifest',str(work/'output/build/manifest.json')],check=True,env=environment)
    subprocess.run([sys.executable,str(work/'tools/test_source_readonly.py')],check=True,env=environment)
    return work/'output/build/ram-root-firmware'

def verify(work,profile):
    import hashlib
    import json
    for name,pin in json.loads((work/'config/base-inputs.json').read_text()).items():
        file=work/name
        if file.is_symlink() or not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest()!=pin['sha256']:raise ValueError('Missing or modified original input: '+name)
    return {'device_tree_bindings_verified':not bool(profile['board']['kernel_bindings']),'base_hashes_verified':True}

def package(work,cc,options):
    import importlib.util
    import json
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    sys.path.insert(0,str(work/'tools'))
    import firmware_package,sd_package,sign_update
    kernel=(work/'output/build/kernel.uImage').read_bytes()
    rootfs=(work/'output/build/ram-root-firmware/rootfs.squashfs').read_bytes()
    images={2:kernel,3:rootfs}
    result={'_images':{'kernel':kernel,'rootfs':rootfs}}
    if options.target in ('sd','both'):result['sd']={'linux.bin':sd_package.build(images)}
    if options.target in ('web','both'):
        if not options.private_key:raise ValueError('Web capsule requires --private-key')
        key=serialization.load_pem_private_key(options.private_key.read_bytes(),password=None)
        if not isinstance(key,Ed25519PrivateKey):raise ValueError('Ed25519 PEM key required')
        public=key.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw)
        if public!=(work/'rootfs-overlay/usr/share/camera/update.pub').read_bytes():raise ValueError('Signing key differs from the firmware trust anchor; see docs/UPDATES.md')
        board=json.loads((work/'config/build.json').read_text())['board'].encode().ljust(32,b'\0')
        result['web']={'camera-firmware.cap':sign_update.envelope(firmware_package.build(images),key,options.sequence,board)}
    return result

def import_assets(work,source,options):
    import hashlib
    import json
    import shutil
    import tempfile
    pins=json.loads((work/'config/base-inputs.json').read_text())
    kernel=source/'kernel.uImage'
    with tempfile.TemporaryDirectory(prefix='fullhan-import-') as tmp:
        root=__import__('pathlib').Path(tmp)/'rootfs'
        subprocess.run(['unsquashfs','-no-progress','-no-xattrs','-d',str(root),str(source/'rootfs.squashfs'),*[name.removeprefix('base/rootfs/') for name in pins if name.startswith('base/rootfs/')]],check=True,stdout=subprocess.DEVNULL)
        for name,pin in pins.items():
            if name=='base/kernel-no-watchdog.uImage':continue
            item=kernel if name=='base/kernel.uImage' else root/name.removeprefix('base/rootfs/')
            if item.is_symlink() or not item.is_file() or hashlib.sha256(item.read_bytes()).hexdigest()!=pin['sha256']:raise ValueError('Stock asset differs from profile: '+name)
            destination=work/name;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(item,destination)
        subprocess.run([sys.executable,str(work/'tools/prepare_watchdog_disabled_kernel.py')],check=True)
        shutil.copyfile(work/'output/kernel/kernel.uImage',work/'base/kernel-no-watchdog.uImage')
    return work/'base'
