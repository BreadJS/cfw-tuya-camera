#!/usr/bin/env python3
"""Offline signature and injected-flash tests; never open MTD or a camera."""
import importlib.util
import json
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import tarfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('release',ROOT/'tools/build_release.py')
release=importlib.util.module_from_spec(spec);spec.loader.exec_module(release)
key=Ed25519PrivateKey.generate()
app=bytearray(b'\xff'*release.APP_BYTES)
app[:96]=bytes(96);app[:4]=b'hsqs';struct.pack_into('<H',app,28,4);struct.pack_into('<Q',app,40,96)
capsule=release.build_signed_capsule(bytes(app),key,7)
assert len(capsule)==5243072
key.public_key().verify(capsule[128:192],capsule[:128])
stock=release.build_stock_sd(bytes(app))
footer=stock[-1024:].decode('ascii')
metadata=dict(line.split('=',1) for line in footer.splitlines() if line)
assert metadata['bundle']==release.STOCK_BUNDLE and metadata['version']=='3.10.57.0'
assert metadata['md5']==hashlib.md5(stock[:-1024],usedforsecurity=False).hexdigest()
with tarfile.open(fileobj=io.BytesIO(stock[:-1024]),mode='r:') as archive:
    entries=archive.getmembers()
    assert [m.name for m in entries]==['usr.sqsh4','usr.sqsh4.md5']
    assert all(m.isreg() and m.uid==0 and m.gid==0 and m.mode==0o644 for m in entries)
    assert archive.extractfile(entries[0]).read()==bytes(app)
    assert archive.extractfile(entries[1]).read().decode().strip()==hashlib.md5(app,usedforsecurity=False).hexdigest()
for sequence in (0,-1,2**64):
    try:release.build_signed_capsule(bytes(app),key,sequence)
    except ValueError:pass
    else:raise AssertionError('invalid sequence accepted')
with tempfile.TemporaryDirectory(prefix='anyka-update-test-') as temp:
    temp=Path(temp)
    (temp/'capsule').write_bytes(capsule)
    (temp/'key').write_bytes(key.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw))
    source=ROOT/'src/update'
    command=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer',
             str(Path(__file__).with_suffix('.c')),str(source/'capsule.c'),str(source/'receipt.c'),
             str(source/'vendor/monocypher.c'),str(source/'vendor/monocypher-ed25519.c'),'-o',str(temp/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(temp/'test'),str(temp/'capsule'),str(temp/'key')],check=True,timeout=20)
    # A boolean readiness flip cannot bypass the artifact/evidence checks.
    evidence={'schema':1,'board_id':release.BOARD,'payload_blake2b':'0'*128,
              'boot_id':'test','media':True,'web':True,'network':True,'app_root_boot':True}
    path=temp/'health.json';path.write_text(json.dumps(evidence))
    try:release.checked_boot_evidence(path,bytes(app))
    except ValueError:pass
    else:raise AssertionError('wrong-image boot evidence accepted')
    evidence.update(payload_blake2b=hashlib.blake2b(app).hexdigest(),
                    boot_id='12345678-1234-1234-1234-123456789abc',app_root_boot=False,
                    boot_mode='ram_app_overlay',app_image_started=True,rootfs_pinned=True)
    path.write_text(json.dumps(evidence));assert release.checked_boot_evidence(path,bytes(app))==evidence
    evidence['boot_mode']='ram_sdk_host';path.write_text(json.dumps(evidence))
    try:release.checked_boot_evidence(path,bytes(app))
    except ValueError:pass
    else:raise AssertionError('loose RAM files misreported as full APP image boot')
print('Python signing/stock APP-only SD container/boot-evidence gate PASS')
