#!/usr/bin/env python3
"""Build the Anyka AP tools from pinned owner-supplied sources, cache locally."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
from profiles import ROOT,load
from workspace import prepare,compiler

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',default='lsc-smart-rotatable');p.add_argument('--cc');p.add_argument('--hostapd-source',required=True,type=Path);p.add_argument('--libnl-source',required=True,type=Path);a=p.parse_args()
    profile=load(a.board)
    if profile['soc']['backend']!='anyka':p.error('This network builder belongs to the Anyka module')
    destination=ROOT/'local/assets/anyka/network'
    if destination.exists():p.error('Network cache already exists; refusing replacement')
    work=prepare(profile,target=ROOT/'output'/a.board/'network-build',include_assets=False)
    cc=shutil.which(a.cc or json.loads((ROOT/'local/toolchains.json').read_text())['anyka'])
    if not cc:p.error('Cross compiler not found')
    subprocess.run([sys.executable,str(work/'tools/build_network.py'),'--cc',str(cc),'--hostapd-source',str(a.hostapd_source.resolve()),'--libnl-source',str(a.libnl_source.resolve())],check=True)
    shutil.copytree(work/'output/network',destination)
    print('Cached network tools:',destination)
