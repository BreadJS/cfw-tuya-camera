#!/usr/bin/env python3
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='picture-isp-') as td:
 binary=Path(td)/'test'
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(Path(__file__).with_suffix('.c')),str(ROOT/'src/native/isp/picture_isp.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
