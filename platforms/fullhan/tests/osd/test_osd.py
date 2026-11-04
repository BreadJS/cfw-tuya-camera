#!/usr/bin/env python3
"""Exact-board packet/font/runtime host fixtures, no ioctls."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-osd-tests-') as td:
 for test,sources in [('timestamp',['timestamp']),('graphv2',['graphv2']),('runtime',['runtime','timestamp','graphv2']),('allocator',['runtime','timestamp','graphv2','../native/vmm/legacy_vmm'])]:
  source=ROOT/'src/osd';binary=Path(td)/test
  subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(source/f'test_{test}.c'),*[str(source/f'{x}.c') for x in sources],'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
