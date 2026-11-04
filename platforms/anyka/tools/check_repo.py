#!/usr/bin/env python3
"""Check source completeness, board boundaries, and public-source exclusions."""
import json
from pathlib import Path
import subprocess
from source_files import ROOT, source_files


def main():
    sources = json.loads((ROOT / 'config/sources.json').read_text())
    for name in sources:
        p = Path(name)
        assert not p.is_absolute() and '..' not in p.parts and (ROOT / p).is_file(), name
        assert not any(word in name for word in ('fh_', '/vmm/', '/mipi/')), name
    board = json.loads((ROOT / 'config/board.json').read_text())
    assert board['board_id'] == 'ak39ev330-mis2006-ptz-8m-v1'
    offset = 0
    for part in board['partitions']:
        assert part['offset'] == offset and part['erase_bytes'] == 4096
        offset += part['bytes']
    assert offset == board['flash_bytes'] == 8 * 1024 * 1024
    for name in ('compile-web.sh', 'compile-sd.sh'):
        subprocess.run(['sh', '-n', str(ROOT / name)], check=True)
    count = 0
    for path in source_files():
        rel = path.relative_to(ROOT)
        assert path.suffix not in ('.bin', '.cap', '.pem', '.key', '.ko', '.so', '.a', '.o'), str(rel)
        if path.suffix in ('.py', '.c', '.h', '.sh') and rel.parts[0] != 'tests':
            text = path.read_text()
            assert str(Path('..') / 'custom-firmware') + '/' not in text, str(rel)
            assert '/' + 'Projects' + '/' + 'CamerHack' + '/' not in text, str(rel)
        count += 1
    print(f'PASS: {count} public source files, standalone paths, Anyka geometry, no vendor/backup/key artifacts.')


if __name__ == '__main__':
    main()
