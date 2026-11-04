#!/usr/bin/env python3
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--directory',type=Path,default=ROOT/'output/ram-boot');parser.add_argument('--runtime-manifest',type=Path)
args=parser.parse_args();out=args.directory
with tempfile.TemporaryDirectory(prefix='camera-raminit-test-') as tmp:
 tmp=Path(tmp);binary=tmp/'raminit-test';crypto=tmp/'crypto.o'
 subprocess.run(['cc','-std=c11','-O0','-c',str(ROOT/'src/ota/vendor/monocypher.c'),'-o',str(crypto)],check=True)
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DRAMINIT_HOST_TEST','-I'+str(out),'-I'+str(ROOT/'src/ota/vendor'),str(ROOT/'ram-boot/raminit.c'),str(crypto),'-o',str(binary)],check=True)
 def run(args):return subprocess.run([str(x) for x in args],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 assert run([binary,'--verify-archive',out/'ROOTFS.CPI']).returncode==0
 bad=tmp/'bad.cpi';data=(out/'ROOTFS.CPI').read_bytes();bad.write_bytes(data[:-1]+bytes([data[-1]^1]));assert run([binary,'--verify-archive',bad]).returncode==1
 bad.write_bytes(data[:-1]);assert run([binary,'--verify-archive',bad]).returncode==1
 dest=tmp/'root';dest.mkdir();p=run([binary,'--extract-test',out/'ROOTFS.CPI',dest]);assert p.returncode==0,p.stderr.decode()+p.stdout.decode()
 manifest=json.loads((args.runtime_manifest or out/'manifest.json').read_text())['rootfs']['files']
 for name,entry in manifest.items():
  path=dest/name
  if 'symlink' in entry:assert path.is_symlink() and os.readlink(path)==entry['symlink'],name
  elif 'sha256' in entry:assert path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest()==entry['sha256'],name
 assert (dest/'usr/share/camera/bootstrap.auth').stat().st_mode&0o777==0o600
 assert run([binary,'--unknown']).returncode==2
 print('PASS RAMINIT: exact archive verification, corruption/truncation rejection, ASan/UBSan extraction, all image file/symlink hashes and private asset mode')
