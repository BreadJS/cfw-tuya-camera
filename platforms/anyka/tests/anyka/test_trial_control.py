#!/usr/bin/env python3
"""Test trial dashboard status and preview ownership without camera access."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='anyka-control-test-') as temp:
    exe = Path(temp) / 'control-test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall',
                    '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-pthread',
                    str(Path(__file__).with_suffix('.c')),
                    str(ROOT / 'src/preview/preview.c'),
                    str(ROOT / 'src/native/rtsp/h264_parameters.c'),
                    str(ROOT / 'src/preview/mp4.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
