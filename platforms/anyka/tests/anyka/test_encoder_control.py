#!/usr/bin/env python3
"""Sanitized host mock of exact Anyka encoder controls; no camera access."""
import os
from pathlib import Path
import subprocess
import tempfile
with tempfile.TemporaryDirectory(prefix='anyka-encoder-test-') as temp:
    exe=Path(temp)/'encoder-test'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(Path(__file__).with_suffix('.c')),'-ldl','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
