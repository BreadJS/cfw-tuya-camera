#!/usr/bin/env python3
"""Build the FAT-root PID1 loader and bounded uncompressed newc RAM filesystem."""
import hashlib,json,os,stat,struct,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def archive(entries):
    result=bytearray()
    for ino,(name,mode,data,major,minor) in enumerate(entries,1):
        n=name.encode()+b'\0';values=(ino,mode,0,0,1,0,len(data),0,0,major,minor,len(n),0)
        result+=b'070701'+''.join('%08x'%x for x in values).encode()+n;result+=b'\0'*((-len(result))%4);result+=data;result+=b'\0'*((-len(result))%4)
    result+=b'070701'+''.join('%08x'%x for x in (0,0,0,0,1,0,0,0,0,0,0,11,0)).encode()+b'TRAILER!!!\0';result+=b'\0'*((-len(result))%512);return bytes(result)
def main():
    out=ROOT/'output/ram-boot';manifest=json.loads((out/'manifest.json').read_text());assert manifest['configuration_storage']=='RAM only'
    with tempfile.TemporaryDirectory(prefix='camera-raminit-') as tmp:
        dest=Path(tmp)/'root';subprocess.run(['unsquashfs','-no-xattrs','-no-progress','-d',str(dest),str(out/'rootfs.squashfs'),'bin','sbin','lib','etc','usr','root','var','proc','sys','mnt','tmp','run'],check=True,stdout=subprocess.PIPE)
        (dest/'dev/pts').mkdir(parents=True)
        entries=[('.',stat.S_IFDIR|0o755,b'',0,0)]
        for p in sorted(dest.rglob('*'),key=lambda p:(len(p.relative_to(dest).parts),str(p.relative_to(dest)))):
            name=str(p.relative_to(dest))
            if p.is_symlink():entries.append((name,stat.S_IFLNK|0o777,os.readlink(p).encode(),0,0))
            elif p.is_dir():entries.append((name,stat.S_IFDIR|(p.stat().st_mode&0o777),b'',0,0))
            elif p.is_file():entries.append((name,stat.S_IFREG|(p.stat().st_mode&0o777),p.read_bytes(),0,0))
        entries += [('dev/console',stat.S_IFCHR|0o600,b'',5,1),('dev/null',stat.S_IFCHR|0o666,b'',1,3)]
        blob=archive(entries);(out/'ROOTFS.CPI').write_bytes(blob)
        header='#define RAMROOT_BYTES %d\nstatic const unsigned char ramroot_hash[64]={%s};\n'%(len(blob),','.join(str(x) for x in hashlib.blake2b(blob).digest()));(out/'ramroot_expected.h').write_text(header)
    compiler=os.environ.get('ARM_CC','/tmp/camera-armv6.aJ9j7F/arm-openipc-linux-musleabi_sdk-buildroot/bin/arm-openipc-linux-musleabi-gcc')
    subprocess.run([compiler,'-std=c11','-Os','-static','-s','-mcpu=arm1176jzf-s','-mfloat-abi=soft','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(out),'-I'+str(ROOT/'src/ota/vendor'),str(ROOT/'ram-boot/raminit.c'),str(ROOT/'src/ota/vendor/monocypher.c'),'-o',str(out/'RAMINIT')],check=True)
    (out/'raminit-prepared.json').write_text(json.dumps({'archive_bytes':len(blob),'archive_sha256':hashlib.sha256(blob).hexdigest(),'entries':len(entries),'binary_sha256':hashlib.sha256((out/'RAMINIT').read_bytes()).hexdigest(),'runtime_booted':False},indent=2)+'\n');print('Prepared RAMINIT and ROOTFS.CPI:',len(blob),'bytes',len(entries),'entries')
if __name__=='__main__':main()
