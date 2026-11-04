#!/usr/bin/env python3
"""Export only the public source tree, never research, binaries or credentials."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from source_files import ROOT, source_files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    version = json.loads((ROOT / 'config/build.json').read_text())['version']
    output = a.output or ROOT / 'output' / ('custom-firmware-ptz-source-' + version + '.zip')
    files = list(source_files())
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            info = zipfile.ZipInfo(str(path.relative_to(ROOT)), (2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o100755 if path.suffix == '.sh' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
    with zipfile.ZipFile(output) as archive:
        assert archive.testzip() is None
    print('Source archive:', output)
    print('SHA256:', hashlib.sha256(output.read_bytes()).hexdigest())
    print('This is a source archive, not an SD installer.')


if __name__ == '__main__':
    main()
