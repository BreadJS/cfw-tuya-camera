#!/usr/bin/env python3
"""Owned fake mappings/MMIO only, sanitizer checks for colour integration."""
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-color-test-') as td:
 for test,sources in [('test_shadow_runtime',['isp_shadow_runtime.c','isp_shadow.c','isp_tone_profile.c']),('test_tone_profile',['isp_tone_profile.c']),('test_shadow',['isp_shadow.c']),('test_awb_mode1',['isp_awb_mode1.c','isp_awb.c']),('test_gamma',['gamma/control.c']),('test_publish',['isp_color_auto.c']),('test_stats',['isp_stats.c','isp_memory.c','isp_shadow.c']),('test_illumination',['../illumination/controller.c','../illumination/ircut.c','../illumination/ir_led.c'])]:
  binary=Path(td)/test;src=ROOT/'src/native/isp'
  subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(Path(__file__).with_name(test+'.c')),*[str(src/x) for x in sources],'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
 subprocess.run(['python3',str(ROOT/'src/native/isp/test_isp_awb.py'),'--sanitize'],check=True)
