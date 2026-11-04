#!/usr/bin/env python3
"""Explicit one-time input migration; production builds do not read old projects."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from profiles import ROOT

def import_project(backend,source):
    source=source.resolve();base=source/'base';destination=ROOT/'local/assets'/backend
    if destination.exists():raise ValueError('Inputs already exist; refusing replacement: '+str(destination))
    if not base.is_dir():raise ValueError('No prepared base inputs: '+str(base))
    destination.mkdir(parents=True)
    # These ignored owner inputs include proprietary binaries, never exported.
    shutil.copytree(base,destination/'base',symlinks=True)
    if backend=='anyka':
        network=source/'output/network'
        if network.is_dir():shutil.copytree(network,destination/'network',symlinks=True)
    files={str(p.relative_to(destination)):{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in destination.rglob('*') if p.is_file()}
    (destination/'import-receipt.json').write_text(json.dumps({'schema':1,'backend':backend,'source':str(source),'files':files},indent=2)+'\n')
    print('Imported',backend,'local assets:',len(files),'files; no build depends on the old directory.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--backend',choices=('fullhan','anyka'),required=True);p.add_argument('--from-project',type=Path,required=True);a=p.parse_args();import_project(a.backend,a.from_project)
