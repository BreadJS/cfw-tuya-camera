#!/usr/bin/env python3
"""Explicitly create a new board signing key; never overwrite an existing key."""
import argparse
import os
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from profiles import ROOT,load

def create(private):
    public=private.with_suffix('.pub')
    if private.exists() or public.exists():raise ValueError('Key or public file already exists; refusing replacement')
    private.parent.mkdir(parents=True,exist_ok=True)
    key=Ed25519PrivateKey.generate()
    data=key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption())
    fd=os.open(private,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as stream:stream.write(data)
    with public.open('xb') as stream:stream.write(key.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw))
    return private,public
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--board',required=True);p.add_argument('--output',type=Path);a=p.parse_args();profile=load(a.board)
    for path in create(a.output or ROOT/'local/keys'/(profile['board']['id']+'.pem')):print(path)
