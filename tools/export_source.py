#!/usr/bin/env python3
"""Export this standalone public project; never include local owner inputs."""
import argparse
import hashlib
from pathlib import Path
import zipfile
from profiles import ROOT

ROOT_FILES={'README.md','LICENSE','Makefile','requirements.txt','.gitignore','compile-web.sh','compile-sd.sh','project.json'}
ROOT_DIRS={'boards','soc','shared','platforms','tools','tests','docs','.github'}
EXCLUDED={'__pycache__','.git','build','node_modules'}

def source_files(root=ROOT):
    candidates=[root/name for name in ROOT_FILES if (root/name).exists()]
    for name in ROOT_DIRS:
        if (root/name).is_dir():candidates.extend((root/name).rglob('*'))
    for path in sorted(candidates):
        relative=path.relative_to(root)
        if relative.parts[0] not in ROOT_DIRS and str(relative) not in ROOT_FILES:continue
        if any(part in EXCLUDED for part in relative.parts):continue
        if path.is_symlink():raise ValueError('Source symlink is not allowed: '+str(relative))
        if not path.is_file():continue
        if path.suffix in ('.pem','.key','.bin','.cap','.ko','.so','.o','.a','.pyc'):raise ValueError('Private/binary artifact in public source: '+str(relative))
        prefix=path.read_bytes()[:40]
        if prefix.startswith((b'\x7fELF',b'-----BEGIN PRIVATE KEY',b'-----BEGIN RSA PRIVATE KEY')):raise ValueError('Executable/private key in public source: '+str(relative))
        yield path

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=ROOT/'output/camera-firmware-source.zip');a=parser.parse_args()
    a.output.parent.mkdir(parents=True,exist_ok=True)
    files=list(source_files())
    with zipfile.ZipFile(a.output,'w',zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            info=zipfile.ZipInfo(str(path.relative_to(ROOT)),(2026,1,1,0,0,0));info.external_attr=(0o100755 if path.suffix=='.sh' else 0o100644)<<16;info.compress_type=zipfile.ZIP_DEFLATED;archive.writestr(info,path.read_bytes())
    with zipfile.ZipFile(a.output) as archive:
        if archive.testzip():raise ValueError('Export ZIP verification failed')
    digest=hashlib.sha256(a.output.read_bytes()).hexdigest()
    a.output.with_suffix(a.output.suffix+'.sha256').write_text(digest+'  '+a.output.name+'\n')
    print('Public source:',a.output,'files='+str(len(files)),'sha256='+digest)
if __name__=='__main__':main()
