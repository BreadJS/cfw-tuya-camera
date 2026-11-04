#!/usr/bin/env python3
"""Real backend with temporary configuration, no camera/hardware operations."""
import json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-display-backend-') as td:
 sources=[ROOT/x for x in json.loads((ROOT/'config/sources.json').read_text()) if x!='src/native/platform/first_frame.c']
 binary=Path(td)/'test'
 objects=[]
 for source in list(sources):
  if source.name in ('monocypher.c','monocypher-ed25519.c'):
   obj=Path(td)/(source.name+'.o');subprocess.run(['cc','-std=c11','-O0','-c',str(source),'-o',str(obj)],check=True);objects.append(obj);sources.remove(source)
 subprocess.run([os.environ.get('CC','cc'),'-DCONTROL_TESTING','-DCAMERA_RAM_BOOT_TEST','-DCAMERA_WATCHDOG_PROBE_LIBRARY','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'src/ota/vendor'),str(Path(__file__).with_name('backend.c')),*map(str,sources),*map(str,objects),'-o',str(binary)],check=True)
 # Application contexts intentionally live until process/device reset.
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
 subprocess.run([str(binary),td],env=env,check=True,timeout=15)
