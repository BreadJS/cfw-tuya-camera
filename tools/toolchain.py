#!/usr/bin/env python3
"""Install a bundled cross compiler offline; verify before extracting."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
from profiles import ROOT,ProfileError,load

def install(profile,cache):
    pin=profile['soc']['toolchain'];name=profile['soc']['id'];destination=cache/name
    archive=(ROOT/pin['archive']).resolve()
    if not archive.is_relative_to((ROOT/'toolchains').resolve()):raise ProfileError('Toolchain archive must be inside the repository toolchains directory')
    if not archive.is_file():raise ProfileError('Bundled toolchain archive is missing: '+str(archive))
    if archive.stat().st_size!=pin['bytes'] or hashlib.sha256(archive.read_bytes()).hexdigest()!=pin['sha256']:raise ProfileError('Bundled toolchain archive differs from pin')
    cache.mkdir(parents=True,exist_ok=True)
    receipt=destination/'verified.json'
    if destination.exists():
        if not receipt.is_file() or json.loads(receipt.read_text()).get('archive_sha256')!=pin['sha256']:raise ProfileError('Existing toolchain lacks a matching completion receipt; move it aside first')
    else:
        with tempfile.TemporaryDirectory(dir=cache,prefix='.extract-') as tmp:
            stage=Path(tmp)/'sdk';stage.mkdir()
            def safe(member,path):
                # This official Fullhan-compatible SDK contains two stale build
                # host symlinks. Only these exact destinations are rewritten.
                if member.issym() and member.linkname.startswith('/'):
                    if member.name.endswith('/bin/fw_setenv') and member.linkname.endswith('/host-uboot-tools/host/bin/fw_printenv'):member=member.replace(linkname='fw_printenv')
                    elif member.name.endswith('/share/gettext') and member.linkname.endswith('/host-gettext-tiny/host/share/gettext-tiny'):member=member.replace(linkname='gettext-tiny')
                    else:raise ProfileError('Unexpected absolute toolchain symlink: '+member.name)
                return tarfile.data_filter(member,path)
            with tarfile.open(archive,'r:*') as source:source.extractall(stage,filter=safe)
            if not (stage/pin['compiler_relative']).is_file():raise ProfileError('Compiler missing from verified archive')
            (stage/'verified.json').write_text(json.dumps({'schema':1,'archive_sha256':pin['sha256'],'soc':name},indent=2)+'\n')
            stage.rename(destination)
    cc=destination/pin['compiler_relative']
    if not cc.is_file():raise ProfileError('Installed compiler is missing')
    settings=ROOT/'local/toolchains.json';settings.parent.mkdir(parents=True,exist_ok=True)
    values=json.loads(settings.read_text()) if settings.exists() else {}
    values[profile['soc']['backend']]=str(cc.resolve());settings.write_text(json.dumps(values,indent=2)+'\n')
    return cc

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',required=True);p.add_argument('--cache',type=Path,default=Path.home()/'.cache/camera-firmware/toolchains');a=p.parse_args()
    print(install(load(a.board),a.cache))
