#!/usr/bin/env python3
"""Check production provider and packetizer bounds without camera access."""
import subprocess,tempfile
from pathlib import Path
with tempfile.TemporaryDirectory(prefix='camera-large-au-') as td:
    binary=Path(td)/'test'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',
        str(Path(__file__).with_suffix('.c')),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
