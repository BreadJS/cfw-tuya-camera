#!/usr/bin/env python3
"""Offline candidate: read-only firmware container -> verified complete RAM root.

No device access, mounting, flash writes, signing or release approval.
"""
import hashlib,json,os,stat,subprocess,tempfile
from pathlib import Path
from prepare_raminit import archive
import firmware_package
ROOT=Path(__file__).resolve().parents[1]

def run(args):
    return subprocess.run([str(x) for x in args],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout

def main():
    source=ROOT/'output/build';out=source/'ram-root-firmware';out.mkdir(exist_ok=True)
    manifest=json.loads((source/'manifest.json').read_text())
    if manifest['configuration_storage']!='persistent JFFS2':raise ValueError('Production runtime required')
    runtime=source/'rootfs.squashfs'
    if hashlib.sha256(runtime.read_bytes()).hexdigest()!=manifest['rootfs']['sha256']:raise ValueError('Stale runtime manifest')
    epoch=manifest['epoch']
    with tempfile.TemporaryDirectory(prefix='camera-ram-root-') as tmp:
        tmp=Path(tmp);root=tmp/'runtime'
        run(['unsquashfs','-no-xattrs','-no-progress','-d',root,runtime,'bin','sbin','lib','etc','usr','root','var','proc','sys','mnt','tmp','run'])
        (root/'dev/pts').mkdir(parents=True)
        entries=[('.',stat.S_IFDIR|0o755,b'',0,0)]
        for p in sorted(root.rglob('*'),key=lambda p:(len(p.relative_to(root).parts),str(p.relative_to(root)))):
            name=str(p.relative_to(root))
            if p.is_symlink():entries.append((name,stat.S_IFLNK|0o777,os.readlink(p).encode(),0,0))
            elif p.is_dir():entries.append((name,stat.S_IFDIR|(p.stat().st_mode&0o777),b'',0,0))
            elif p.is_file():entries.append((name,stat.S_IFREG|(p.stat().st_mode&0o777),p.read_bytes(),0,0))
            else:raise ValueError('Unexpected runtime entry '+name)
        entries.extend([('dev/console',stat.S_IFCHR|0o600,b'',5,1),('dev/null',stat.S_IFCHR|0o666,b'',1,3)])
        blob=archive(entries);(out/'ROOTFS.CPI').write_bytes(blob)
        (out/'ramroot_expected.h').write_text('#define RAMROOT_BYTES %d\nstatic const unsigned char ramroot_hash[64]={%s};\n'%(len(blob),','.join(str(x) for x in hashlib.blake2b(blob).digest())))
        cc=os.environ.get('ARM_CC','/tmp/camera-armv6.aJ9j7F/arm-openipc-linux-musleabi_sdk-buildroot/bin/arm-openipc-linux-musleabi-gcc')
        run([cc,'-std=c11','-Os','-static','-s','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror','-DCAMERA_NOR_RAM_ROOT','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(out),'-I'+str(ROOT/'src/ota/vendor'),ROOT/'ram-boot/raminit.c',ROOT/'src/ota/vendor/monocypher.c','-o',out/'ram-root-init'])
        wrapper=tmp/'wrapper';wrapper.mkdir()
        for name in ('sbin','dev','RAMROOT'):(wrapper/name).mkdir()
        (wrapper/'sbin/init').write_bytes((out/'ram-root-init').read_bytes());(wrapper/'sbin/init').chmod(0o755)
        (wrapper/'ROOTFS.CPI').write_bytes(blob);(wrapper/'ROOTFS.CPI').chmod(0o600)
        image=out/'rootfs.squashfs';image.unlink(missing_ok=True)
        run(['mksquashfs',wrapper,image,'-noappend','-comp','xz','-b','131072','-all-root','-root-mode','755','-no-xattrs','-mkfs-time',epoch,'-all-time',epoch,'-processors','1','-p','dev/console c 600 0 0 5 1','-p','dev/null c 666 0 0 1 3'])
        data=firmware_package.check_image(3,image.read_bytes());image.write_bytes(data)
        if run(['unsquashfs','-cat',image,'ROOTFS.CPI'])!=blob:raise ValueError('Packed runtime differs')
        if run(['unsquashfs','-cat',image,'sbin/init'])!=(out/'ram-root-init').read_bytes():raise ValueError('Packed PID1 differs')
        payload=firmware_package.build({3:data});(out/'rootfs-ramboot.update.bin').write_bytes(payload)
        kernel=(source/'kernel.uImage').read_bytes()
        if hashlib.sha256(kernel).hexdigest()!=manifest['kernel']['sha256']:raise ValueError('Stale kernel manifest')
        complete=firmware_package.build({2:kernel,3:data});(out/'firmware-kernel-rootfs.update.bin').write_bytes(complete)
        report={'board':manifest['board'],'boot_design':'read-only SquashFS container -> verified tmpfs root; old container detached before services','configuration_storage':'persistent JFFS2','rootfs_bytes':len(data),'partition_budget':0x550000,'archive_bytes':len(blob),'archive_entries':len(entries),'archive_sha256':hashlib.sha256(blob).hexdigest(),'runtime_manifest_sha256':hashlib.sha256((source/'manifest.json').read_bytes()).hexdigest(),'kernel':manifest['kernel'],'package':firmware_package.inspect(payload),'complete_package':firmware_package.inspect(complete),'runtime_boot_tested':'FAT-to-RAM variant only','container_boot_tested':False,'physical_apply_enabled':False,'ready_to_flash':False}
        (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
        print('Prepared RAM-root firmware container:',len(data),'bytes; candidate only, not flashed')

if __name__=='__main__':main()
