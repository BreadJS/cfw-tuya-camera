#!/usr/bin/env python3
import subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-encoder-settings-') as td:
 binary=Path(td)/'test'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
  '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(Path(__file__).with_suffix('.c')),
  str(ROOT/'src/native/capture/fh_encoder_settings.c'),str(ROOT/'src/native/capture/fh_capture.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
