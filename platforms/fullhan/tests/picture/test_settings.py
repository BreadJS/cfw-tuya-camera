#!/usr/bin/env python3
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-picture-settings-') as td:
 binary=Path(td)/'test';config=Path(td)/'config';config.mkdir(mode=0o700)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(Path(__file__).with_suffix('.c')),
  str(ROOT/'src/picture/settings.c'),str(ROOT/'src/native/isp/picture_isp.c'),str(ROOT/'src/native/capture/fh_encoder_settings.c'),str(ROOT/'src/native/control/http.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(config)],check=True)
