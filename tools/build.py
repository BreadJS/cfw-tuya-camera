#!/usr/bin/env python3
"""Select a board, validate wiring and compose its shared services/backend."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from profiles import ROOT,ProfileError,load
from workspace import prepare,compiler
from packages import publish

def module(profile):
    name=profile['soc']['backend'];file=ROOT/'platforms'/name/profile['module']['adapter']
    if file.parent != ROOT/'platforms'/name:raise ProfileError('Adapter must be in its module directory')
    spec=importlib.util.spec_from_file_location('camera_backend_'+name,file)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['list','configure','check','application','image','candidate','release'])
    p.add_argument('--config','--board',dest='board',default='lsc-smart-indoor',help='Board configuration name or JSON file (--board is an alias)')
    p.add_argument('--soc',help='Require the selected board to use this SoC')
    p.add_argument('--set',action='append',default=[],metavar='PATH=JSON')
    p.add_argument('--disable',action='append',default=[],metavar='FEATURE')
    p.add_argument('--cc',default=os.environ.get('ARM_CC'))
    p.add_argument('--private-key',type=Path)
    p.add_argument('--trust-public-key',type=Path,help='Bootstrap a Fullhan image with this raw32-byte public anchor; existing cameras still require their current trust key')
    p.add_argument('--target',choices=['web','sd','both'],default='both')
    p.add_argument('--sequence',type=int,default=2)
    p.add_argument('--boot-evidence',type=Path)
    a=p.parse_args()
    if not 0<a.sequence<2**64:p.error('sequence must be 1 through 2^64-1')
    try:
        if a.action=='list':
            for path in sorted((ROOT/'boards').glob('*.json')):
                profile=load(path);print(profile['board']['id']+'\t'+profile['soc']['id']+'\t'+profile['board']['name'])
            return 0
        overrides={}
        for value in a.set:
            key,separator,raw=value.partition('=')
            if not separator:raise ProfileError('--set requires PATH=JSON, e.g. gpio.ir_led.pin=4')
            overrides[key]=json.loads(raw)
        profile=load(a.board,overrides=overrides,features={f:False for f in a.disable})
        if a.soc and a.soc!=profile['soc']['id']:raise ProfileError('--soc does not match the selected board')
        if a.action=='check':
            print(json.dumps({'board':profile['board'],'soc':profile['soc'],'profile_sha256':profile['sha256']},indent=2));return 0
        lock_dir=ROOT/'output'/profile['board']['id'];lock_dir.mkdir(parents=True,exist_ok=True)
        build_lock=(lock_dir/'.build.lock').open('a')
        try:fcntl.flock(build_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:raise ProfileError('Another build is using this board output')
        work=prepare(profile)
        if a.trust_public_key:
            anchor=work/'rootfs-overlay/usr/share/camera/update.pub'
            data=a.trust_public_key.read_bytes()
            if len(data)!=32 or not anchor.exists():raise ProfileError('--trust-public-key requires a raw32-byte key and a backend with a file trust anchor')
            anchor.write_bytes(data)
        if a.action=='configure':print(work);return 0
        settings=ROOT/'local/toolchains.json'
        configured=json.loads(settings.read_text()) if settings.exists() else {}
        real=a.cc or configured.get(profile['soc']['backend'])
        if not real:raise ProfileError('Supply --cc or local/toolchains.json; see README.md')
        cc=compiler(work,real);compiler(work,'cc',name='profile-host-cc')
        verification=module(profile).verify(work,profile)
        (work.parent/'hardware-bindings.json').write_text(json.dumps(verification,indent=2)+'\n')
        a.epoch=profile['project']['build_epoch']
        backend=module(profile)
        if a.action in ('candidate','release'):
            backend.image(work,cc,a)
            result=publish(profile,work,backend.package(work,cc,a),a)
        else:result=getattr(backend,a.action)(work,cc,a)
        receipt={'schema':1,'board':profile['board']['id'],'soc':profile['soc']['id'],'version':profile['project']['version'],'profile_sha256':profile['sha256'],'action':a.action,'artifacts':str(result.relative_to(ROOT)),'flash_ready':a.action=='release','hardware_verified':a.action=='release'}
        (work.parent/'build-result.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print('Build complete:',result)
        return 0
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        print('Build failed:',error,file=sys.stderr);return 2

if __name__=='__main__':sys.exit(main())
