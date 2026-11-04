#!/usr/bin/env python3
"""Fetch a pinned cross compiler into the local cache; verify before extracting."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request
from profiles import ROOT,ProfileError,load

def install(profile,cache):
    pin=profile['soc']['toolchain'];name=profile['soc']['id'];destination=cache/name
    archive=cache/(pin['sha256']+'.tar');cache.mkdir(parents=True,exist_ok=True)
    if not archive.exists():
        with tempfile.NamedTemporaryFile(dir=cache,delete=False) as stream:
            temporary=Path(stream.name)
            try:
                with urllib.request.urlopen(pin['url'],timeout=60) as response:shutil.copyfileobj(response,stream)
            except BaseException:temporary.unlink(missing_ok=True);raise
        if temporary.stat().st_size!=pin['bytes'] or hashlib.sha256(temporary.read_bytes()).hexdigest()!=pin['sha256']:
            temporary.unlink();raise ProfileError('Toolchain download does not match its pinned size/hash')
        temporary.rename(archive)
    if archive.stat().st_size!=pin['bytes'] or hashlib.sha256(archive.read_bytes()).hexdigest()!=pin['sha256']:raise ProfileError('Cached toolchain archive differs from pin')
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
