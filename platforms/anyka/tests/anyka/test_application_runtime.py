#!/usr/bin/env python3
"""Host-only APP provenance/receipt checks; no camera, mount or MTD operations."""
import os
from pathlib import Path
import subprocess
import tempfile
with tempfile.TemporaryDirectory(prefix='anyka-runtime-test-') as temp:
    binary=Path(temp)/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(Path(__file__).with_suffix('.c')),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary),temp],check=True,timeout=10)
