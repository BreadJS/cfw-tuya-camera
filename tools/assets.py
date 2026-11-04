#!/usr/bin/env python3
"""Import pinned original images without requiring either legacy source project."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from profiles import ROOT,load
from workspace import prepare
from build import module

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',required=True);p.add_argument('--input-dir',required=True,type=Path);a=p.parse_args()
    profile=load(a.board);destination=ROOT/'local/assets'/profile['soc']['backend']
    if destination.exists():p.error('Local assets already exist; refusing to replace them')
    work=prepare(profile,target=ROOT/'output'/a.board/'asset-import',include_assets=False)
    base=module(profile).import_assets(work,a.input_dir.resolve(),a)
    destination.mkdir(parents=True);shutil.copytree(base,destination/'base',symlinks=True)
    inventory={str(path.relative_to(destination)):{'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in destination.rglob('*') if path.is_file()}
    (destination/'import-receipt.json').write_text(json.dumps({'schema':1,'board':a.board,'files':inventory},indent=2)+'\n')
    print('Imported verified owner assets:',destination)
