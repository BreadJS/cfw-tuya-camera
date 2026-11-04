#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ptz-calibration-test-') as tmp:
    exe=Path(tmp)/'test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',str(ROOT/'tests/anyka/test_calibration.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
