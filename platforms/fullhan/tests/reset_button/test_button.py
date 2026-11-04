#!/usr/bin/env python3
"""Synthetic GPIO samples only; no live pins, config writes or reboot."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='camera-reset-button-') as temp:
    binary=Path(temp)/'test'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(Path(__file__).with_name('driver.c')),
                    str(ROOT/'src/reset/button.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
