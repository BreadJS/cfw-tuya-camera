#!/usr/bin/env python3
"""Offline-only isolated camera image construction. Never accesses UART/MTD."""
import argparse,datetime,hashlib,importlib.util,json,os,re,shutil,subprocess,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ALIASES={'ld-uClibc.so.0':'ld-uClibc-0.9.33.2.so','libc.so.0':'libuClibc-0.9.33.2.so','libcrypt.so.0':'libcrypt-0.9.33.2.so','libm.so.0':'libm-0.9.33.2.so','libpthread.so.0':'libpthread-0.9.33.2.so','librt.so.0':'librt-0.9.33.2.so','libdl.so.0':'libdl-0.9.33.2.so','libresolv.so.0':'libresolv-0.9.33.2.so','libutil.so.0':'libutil-0.9.33.2.so'}
MODULES=('ssv6x5x.ko','vmm.ko','rtvbus.ko','rtthread_arc_FH8626V100.bin','media_process.ko','isp.ko','enc.ko','jpeg.ko','bgm.ko','vbus_ac.ko')
FORBIDDEN=('dgiot','daemon','cold_guard','watchdog_guard','libIOTCAPIs.so','libAVAPIs.so','libsCHL.so','app_init.sh','custom.sh','inetd')
def run(args):
    p=subprocess.run([str(x) for x in args],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    if p.returncode:raise RuntimeError('Command failed: '+str(args[0])+'\n'+p.stderr[-6000:]+p.stdout[-1000:])
    return p.stdout

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def copy(source,destination,mode=0o644):
    destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,destination);destination.chmod(mode)
def write(path,text,mode=0o644):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text);path.chmod(mode)
def link(root,name,target):
    path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to(target)
def needed(path):return re.findall(r'Shared library: \[([^\]]+)\]',run(['readelf','-d',path]))
def audit(stage):
    entries={}
    for path in sorted(stage.rglob('*')):
        name=str(path.relative_to(stage))
        if path.name in FORBIDDEN:raise ValueError('Forbidden shipped component: '+name)
        if path.is_symlink():entries[name]={'symlink':os.readlink(path)}
        elif path.is_file():entries[name]={'bytes':path.stat().st_size,'sha256':digest(path),'mode':oct(path.stat().st_mode&0o777)}
    for name in ('etc/init.d/rcS','usr/libexec/camera/load-hardware','usr/libexec/camera/start-network','usr/libexec/camera/start-service','usr/libexec/camera/start-telnet','usr/libexec/camera/dhcp-hook'):
        run(['sh','-n',stage/name])
    boot='\n'.join((stage/x).read_text() for x in ('etc/inittab','etc/init.d/rcS','usr/libexec/camera/load-hardware','usr/libexec/camera/start-network','usr/libexec/camera/start-service','usr/libexec/camera/start-telnet'))
    for pattern in ('/usr/init/app_init.sh','/usr/bin/dgiot','/usr/bin/daemon','/dev/watchdog','cold_guard','watchdog_guard','custom.sh'):
        if pattern in boot:raise ValueError('Forbidden boot action: '+pattern)
    if (stage/'etc/passwd').read_text()!='root:x:0:0:root:/root:/bin/sh\n':raise ValueError('Unexpected account')
    if 'INTERP' in run(['readelf','-l',stage/'usr/sbin/camera-service']):raise ValueError('Camera service is not static')
    if 'INTERP' in run(['readelf','-l',stage/'usr/libexec/camera/telnet-login']):raise ValueError('Telnet login is not static')
    telnet=(stage/'usr/libexec/camera/start-telnet').read_text()
    if 'exec /bin/busybox telnetd -F -p '+str(json.loads((ROOT/'config/board-profile.json').read_text())['ports']['telnet'])+' -l /usr/libexec/camera/telnet-login' not in telnet:raise ValueError('Unexpected Telnet listener/authentication command')
    return entries

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-rootfs',type=Path,default=ROOT/'base/rootfs')
    parser.add_argument('--kernel',type=Path,default=ROOT/'base/kernel-no-watchdog.uImage')
    parser.add_argument('--cc',default=os.environ.get('ARM_CC','/tmp/camera-armv6.aJ9j7F/arm-openipc-linux-musleabi_sdk-buildroot/bin/arm-openipc-linux-musleabi-gcc'))
    parser.add_argument('--epoch',type=int,default=None)
    parser.add_argument('--application-only',action='store_true')
    parser.add_argument('--ram-boot-test',action='store_true',help='SD-root test build with RAM-only config and guarded init')
    args=parser.parse_args();out=ROOT/'output'/('ram-boot' if args.ram_boot_test else 'build');out.mkdir(parents=True,exist_ok=True)
    epoch=args.epoch
    build_config=json.loads((ROOT/'config/build.json').read_text());version=build_config.get('version','0.1.0')
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9._-]+)?',version):raise ValueError('Invalid release version')
    if epoch is None:epoch=int(os.environ.get('SOURCE_DATE_EPOCH',str(json.loads((ROOT/'config/build.json').read_text())['epoch'])))
    if epoch<1577836800:raise ValueError('Build timestamp must be2020 or later')
    stock=args.stock_rootfs.resolve()
    if len((ROOT/'rootfs-overlay/usr/share/camera/update.pub').read_bytes())!=32:raise ValueError('Public update key must be32 bytes')
    spec=importlib.util.spec_from_file_location('firmware_package',ROOT/'tools/firmware_package.py');package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
    kernel=package.check_image(2,args.kernel.read_bytes())
    print('Building isolated static ARM camera-service...',flush=True)
    run(['python3',ROOT/'src/native/control/generate_ui.py'])
    sources=json.loads((ROOT/'config/sources.json').read_text())
    if any('/watchdog/' in x or Path(x).name in ('cold_guard.c','watchdog_guard.c') for x in sources):
        raise ValueError('Watchdog keeper source in service build')
    run([args.cc,'-DCAMERA_WATCHDOG_PROBE_LIBRARY','-DCAMERA_BUILD_VERSION='+json.dumps(version),*(['-DCAMERA_RAM_BOOT_TEST'] if args.ram_boot_test else []),'-std=c11','-Os','-static','-pthread','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src/ota/vendor'),*[ROOT/x for x in sources],'-o',out/'camera-service'])
    run([args.cc,'-std=c11','-Os','-static','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror',ROOT/'src/time/camera_time.c','-o',out/'camera-time'])
    run([args.cc,'-DCAMERA_CONFIG_RESET_MAIN','-std=c11','-Os','-static','-s','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror',ROOT/'src/reset/config_reset.c','-o',out/'config-reset'])
    run([args.cc,'-std=c11','-Os','-static','-s','-pthread','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror',ROOT/'src/telnet/login.c',ROOT/'src/native/control/credentials.c','-o',out/'telnet-login'])
    installer=['ram_install_main.c','ram_install.c','ota_linux_mtd.c','ota_apply_plan.c','boot_confirm.c','ota_core.c','watchdog_clock_probe.c','vendor/monocypher.c','vendor/monocypher-ed25519.c']
    run([args.cc,'-DCAMERA_WATCHDOG_PROBE_LIBRARY','-std=c11','-Os','-static','-s','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src/ota/vendor'),*[ROOT/'src/ota'/x for x in installer],'-o',out/'firmware-install'])
    if args.application_only:
        print('Built static application and helpers:',out,flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='camera-firmware-build-') as temporary:
        stage=Path(temporary)/'rootfs';stage.mkdir()
        for name in ('bin','sbin','lib','dev','dev/pts','proc','sys','var','etc/conf','etc/init.d','etc/udev/rules.d','root','mnt/sdcard','usr/sbin','usr/modules','usr/libexec/camera','usr/share/camera'):(stage/name).mkdir(parents=True,exist_ok=True)
        for name in ('bin/busybox','bin/wpa_supplicant','bin/wpa_cli','bin/hostapd','sbin/udevd','sbin/udevadm'):copy(stock/name,stage/name,0o755)
        requirements=['ld-uClibc.so.0']
        for name in ('bin/busybox','bin/wpa_supplicant','bin/wpa_cli','bin/hostapd','sbin/udevd','sbin/udevadm'):requirements+=needed(stock/name)
        resolved=set()
        while requirements:
            soname=requirements.pop()
            if soname in resolved:continue
            resolved.add(soname);real=ALIASES.get(soname,soname);source=stock/'lib'/real
            if not source.is_file():raise ValueError('Missing required runtime library: '+soname)
            copy(source,stage/'lib'/real,0o755)
            if real!=soname:link(stage,'lib/'+soname,real)
            requirements+=needed(source)
        for module in MODULES:copy(stock/'usr/modules'/module,stage/'usr/modules'/module)
        copy(stock/'etc/ssv6x5x-wifi.cfg',stage/'etc/ssv6x5x-wifi.cfg')
        for name in ('[','[[','ash','sh','awk','cat','chmod','cp','date','dmesg','echo','false','ifconfig','insmod','kill','ls','mkdir','mknod','mount','mv','ping','printf','ps','reboot','rm','route','sed','sleep','sync','test','touch','true','udhcpc','udhcpd','umount'):link(stage,'bin/'+name,'busybox')
        link(stage,'sbin/init','../bin/busybox');link(stage,'tmp','var/tmp');link(stage,'run','var/run');link(stage,'etc/resolv.conf','../var/nm/resolv.conf')
        overlay=ROOT/'rootfs-overlay'
        for source in sorted(overlay.rglob('*')):
            if source.is_file():
                name=source.relative_to(overlay);mode=0o755 if str(name).startswith('usr/libexec/') or str(name)=='etc/init.d/rcS' else 0o644
                if source.name in ('bootstrap.auth','isp.seed'):mode=0o600
                copy(source,stage/name,mode)
        if args.ram_boot_test:
            for source in (ROOT/'ram-boot/rootfs-overlay').rglob('*'):
                if source.is_file():
                    name=source.relative_to(ROOT/'ram-boot/rootfs-overlay')
                    copy(source,stage/name,0o755 if str(name)=='etc/init.d/rcS' else 0o600)
        copy(out/'camera-service',stage/'usr/sbin/camera-service',0o755)
        copy(out/'camera-time',stage/'usr/sbin/camera-time',0o755)
        copy(out/'config-reset',stage/'usr/libexec/camera/config-reset',0o755)
        copy(out/'telnet-login',stage/'usr/libexec/camera/telnet-login',0o755)
        copy(out/'firmware-install',stage/'usr/libexec/camera/firmware-install',0o755)
        write(stage/'etc/passwd','root:x:0:0:root:/root:/bin/sh\n')
        write(stage/'etc/shadow','root:!:0:0:99999:7:::\n',0o600)
        write(stage/'etc/group','root:x:0:\n')
        write(stage/'etc/hosts','127.0.0.1 localhost camera\n')
        write(stage/'etc/profile','export PATH=/bin:/sbin:/usr/bin:/usr/sbin\n')
        write(stage/'etc/udev/udev.conf','udev_root="/dev"\nudev_rules="/etc/udev/rules.d"\nudev_log="err"\n')
        write(stage/'etc/udev/rules.d/10-camera.rules','KERNEL=="rtc0", NAME="rtc"\nKERNEL=="null|zero|random|urandom", MODE="0666"\n')
        write(stage/'usr/share/camera/build-time-utc',datetime.datetime.fromtimestamp(epoch,datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')+'\n')
        write(stage/'etc/os-release','NAME="Camera Local Firmware"\nID=camerhack\nVERSION_ID="'+version+'"\n')
        for path in stage.rglob('*'):
            if path.is_dir() and not path.is_symlink():path.chmod(0o755)
        (stage/'root').chmod(0o700)
        entries=audit(stage)
        if args.ram_boot_test:
            boot=(stage/'etc/init.d/rcS').read_text()
            if 'mount -t jffs2' in boot or '/dev/mtdblock5' in boot:raise ValueError('NOR config mount in RAM trial')
            if 'mount -t tmpfs' not in boot or 'camera_sd_test=1' not in boot:raise ValueError('Missing RAM trial guards')
        print('Building and inspecting XZ SquashFS...',flush=True)
        image=out/'rootfs.squashfs';image.unlink(missing_ok=True)
        run(['mksquashfs',stage,image,'-noappend','-comp','xz','-b','131072','-all-root','-root-mode','755','-no-xattrs','-mkfs-time',epoch,'-all-time',epoch,'-processors','1','-p','dev/console c 600 0 0 5 1','-p','dev/null c 666 0 0 1 3'])
        rootfs=package.check_image(3,image.read_bytes());image.write_bytes(rootfs)
        check=Path(temporary)/'inspect'
        # List image metadata to verify ownership, device nodes and private assets.
        listing=run(['unsquashfs','-lln',image]);(out/'rootfs-files.txt').write_text(listing)
        for name in FORBIDDEN:
            if any(line.rsplit(' ',1)[-1].split('/')[-1]==name for line in listing.splitlines()):raise ValueError('Forbidden image entry: '+name)
        run(['unsquashfs','-no-xattrs','-no-progress','-d',check,image,'bin','sbin','lib','etc','usr','root','var','proc','sys','mnt','tmp','run'])
        for name,entry in entries.items():
            path=check/name
            if 'sha256' in entry and (not path.is_file() or digest(path)!=entry['sha256']):raise ValueError('Packed file mismatch: '+name)
            if 'symlink' in entry and (not path.is_symlink() or os.readlink(path)!=entry['symlink']):raise ValueError('Packed symlink mismatch: '+name)
        for member in ('etc/inittab','etc/init.d/rcS','usr/libexec/camera/start-service','usr/share/camera/bootstrap.auth'):
            extracted=subprocess.run(['unsquashfs','-cat',str(image),member],check=True,stdout=subprocess.PIPE).stdout
            if extracted!=(stage/member).read_bytes():raise ValueError('SquashFS content mismatch: '+member)
        (out/'kernel.uImage').write_bytes(kernel)
        payload=package.build({3:rootfs});(out/'rootfs-candidate.update.bin').write_bytes(payload);details=package.inspect(payload)
        kernel_hash=hashlib.sha256(kernel).hexdigest();patched=False
        patch_manifest=ROOT/'config/kernel-patch.json'
        if patch_manifest.is_file():patched=json.loads(patch_manifest.read_text())['kernel_uimage_sha256']==kernel_hash
        report={'board':build_config['board'],'epoch':epoch,'configuration_storage':'RAM only' if args.ram_boot_test else 'persistent JFFS2','test_mode':'sd-root' if args.ram_boot_test else 'production-candidate','ready_to_flash':False,'application':{'sha256':digest(out/'camera-service'),'continuous_service':True,'userspace_watchdog':False},'kernel':{'sha256':kernel_hash,'source':'watchdog-init patched stock compatibility kernel' if patched else 'unchanged stock compatibility kernel','watchdog_driver':'registration replaced by vendor clock pause; hardware validation required' if patched else 'still built in; never opened by new userspace; disable/rebuild gate pending'},'rootfs':{'bytes':len(rootfs),'partition_budget':0x550000,'sha256':digest(image),'files':entries},'package':details,'release_gates':json.loads((ROOT/'config/release-gates.json').read_text()),'retained_runtime_libraries':sorted(resolved),'retained_hardware_files':list(MODULES),'excluded_components':list(FORBIDDEN),'device_config_included':False}
        (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
        (out/'SHA256SUMS').write_text(''.join(digest(out/name)+'  '+name+'\n' for name in ('camera-service','rootfs.squashfs','kernel.uImage','rootfs-candidate.update.bin')))
        print('Built rootfs:',len(rootfs),'bytes /',0x550000,'budget. Candidate only; no device accessed.',flush=True)
if __name__=='__main__':
    try:main()
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
