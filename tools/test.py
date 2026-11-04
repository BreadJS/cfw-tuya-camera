#!/usr/bin/env python3
"""Offline shared and backend regressions. Synthetic drivers only; no device IO."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
from profiles import ROOT,load
from workspace import prepare,compiler

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',action='append');a=p.parse_args()
    subprocess.run([sys.executable,'-m','unittest','discover','-s',str(ROOT/'tests'),'-v'],check=True)
    subprocess.run(['node',str(ROOT/'tests/test_picture_save.js')],check=True)
    for name in a.board or ['lsc-smart-indoor','lsc-smart-rotatable']:
        profile=load(name);work=prepare(profile,target=ROOT/'output'/name/'host-tests',include_assets=False)
        binpath=work/'host-bin';binpath.mkdir();cc=compiler(work,'cc',name='host-bin/cc')
        env=dict(os.environ,CC=str(cc),PATH=str(binpath)+os.pathsep+os.environ['PATH'])
        subprocess.run([sys.executable,str(work/'src/native/control/generate_ui.py')],check=True,env=env)
        common=['src/native/rtsp/tests/test_sender_report.py','src/native/rtsp/tests/test_backpressure.py','src/native/rtsp/tests/test_client_failures.py','src/preview/tests/test_preview.py','tests/display/test_settings.py']
        tests=profile['module']['host_tests']+common
        for test in tests:
            print(name+': '+test,flush=True)
            subprocess.run([sys.executable,str(work/test)],check=True,env=env,cwd=work,timeout=180)
        print(name+': offline regressions passed',flush=True)
if __name__=='__main__':main()
