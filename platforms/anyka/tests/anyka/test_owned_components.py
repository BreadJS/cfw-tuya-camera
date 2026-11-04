#!/usr/bin/env python3
"""Sanitized owned application tests; synthetic SDK/sysfs, never a camera."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
common=['src/native/control/http.c']
cases={
 'board_control':[],
 'board_hold':[],
 'board_settings':[],
 'encoder_control':[],
 'encoder_executor':[],
 'sensor_timing':[],
 'advanced_picture':['src/anyka/sensor_timing.c'],
 'camera_time':[],
 'login_shell':[],
 'application':common+['src/anyka/board_settings.c','src/anyka/reset_button.c','src/anyka/video_mode.c'],
 'management':common+['src/native/control/credentials.c','src/display/settings.c','src/native/rtsp/h264_parameters.c','src/preview/preview.c','src/preview/mp4.c','src/anyka/advanced_picture.c','src/anyka/sensor_timing.c'],
}
subprocess.run(['python3',str(ROOT/'src/native/control/generate_ui.py')],check=True)
with tempfile.TemporaryDirectory(prefix='ptz-owned-components-') as temp:
 for name,sources in cases.items():
  exe=Path(temp)/name
  subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',str(ROOT/'tests/anyka'/('test_'+name+'.c')),*[str(ROOT/p) for p in sources],'-ldl','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=20)

subprocess.run(['node',str(ROOT/'tests/anyka/test_picture_ui.js')],check=True,timeout=20)
subprocess.run(['node',str(ROOT/'tests/anyka/test_ptz_ui.js')],check=True,timeout=20)
subprocess.run(['python3',str(ROOT/'tests/anyka/test_video_mode.py')],check=True,timeout=30)
