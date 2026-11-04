#!/usr/bin/env python3
"""Exercise the production SDK binder with simulated symbols, never a device."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='anyka-sdk-api-') as temp:
    out = Path(temp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall',
                    '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(Path(__file__).with_suffix('.c')),
                    str(ROOT / 'src/anyka/sdk_api.c'), '-ldl', '-o', str(out)], check=True)
    subprocess.run([str(out)], check=True, timeout=10)
