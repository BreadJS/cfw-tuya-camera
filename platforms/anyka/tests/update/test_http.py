#!/usr/bin/env python3
"""Host integration with an explicit fake verifier; never executes installer/MTD."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='anyka-update-http-') as temp:
    temp=Path(temp)
    installer=temp/'verifier-mock'
    installer.write_text('#!/bin/sh\n[ "$1" = --verify ] || exit 99\n[ -z "${LD_PRELOAD:-}" ] && [ -z "${LD_LIBRARY_PATH:-}" ] || exit 98\n[ ! -e "$0.fail" ]\n')
    installer.chmod(0o700)
    source=ROOT/'src/update'
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    str(Path(__file__).with_suffix('.c')),str(source/'capsule.c'),str(source/'receipt.c'),
                    str(source/'vendor/monocypher.c'),str(source/'vendor/monocypher-ed25519.c'),
                    '-pthread','-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test'),str(installer),str(temp)],check=True,timeout=20)
