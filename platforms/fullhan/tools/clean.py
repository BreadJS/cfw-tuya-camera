#!/usr/bin/env python3
"""Remove generated intermediates only; retain distribution packages and inputs."""
import shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
for name in ('output/build', 'output/ram-boot', 'output/kernel'):
    path = ROOT / name
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
for path in list(ROOT.rglob('__pycache__')):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
print('Removed build intermediates and Python caches; output/release retained.')
