#!/usr/bin/env python3
"""Remove regenerable build products; never remove vendor assets or investigation."""
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[1]
for name in ('host', 'armv5', 'build'):
    path = root / 'output' / name
    if path.exists():
        shutil.rmtree(path)
(root / 'src/native/control/ui.h').unlink(missing_ok=True)
print('Removed regenerable build products. Release/source archives retained.')
