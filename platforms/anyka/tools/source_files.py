"""Canonical public source inventory shared by validation and source export."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {'investigation', 'output', '.git', '__pycache__'}


def source_files(root=ROOT):
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        if any(part in EXCLUDED for part in rel.parts):
            continue
        if rel.parts[:2] in (('base', 'vendor'), ('base', 'sdk')):
            continue
        if str(rel) == 'src/native/control/ui.h':
            continue
        if path.is_symlink():
            raise ValueError('Unexpected source symlink: ' + str(rel))
        if path.is_file():
            if path.suffix in ('.pem', '.key', '.bin', '.cap', '.ko', '.so', '.a', '.o'):
                raise ValueError('Binary/private artifact in public source: ' + str(rel))
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    raise ValueError('ELF binary in public source: ' + str(rel))
            yield path
