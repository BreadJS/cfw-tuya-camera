#!/usr/bin/env python3
"""No hardware access: strict parsers, metadata policy and unsupported-host refusal."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class Runtime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-boot-runtime-build-');b=Path(cls.build.name);cls.binary=b/'driver';src=ROOT/'src/ota';cc=os.environ.get('CC','cc');objects=[]
        for name in ('monocypher.c','monocypher-ed25519.c'):
            obj=b/(name+'.o');subprocess.run([cc,'-std=c11','-O0','-c',str(src/'vendor'/name),'-o',str(obj)],check=True);objects.append(str(obj))
        subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(src/'vendor'),str(Path(__file__).with_name('driver.c')),str(src/'boot_confirm.c'),str(src/'ota_apply_plan.c'),str(src/'ota_core.c'),*objects,'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='camera-boot-runtime-case-');self.path=Path(self.temp.name)/'state';self.path.mkdir(mode=0o700)
    def tearDown(self):self.temp.cleanup()
    def run_case(self,mode,*args):
        r=subprocess.run([str(self.binary),mode,*map(str,args)],capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stdout+r.stderr);return r.stdout.strip()
    def write(self,data=bytes(240)):
        p=self.path/'install-pending';p.write_bytes(data);p.chmod(0o600);return p
    def test_strict_parsers_and_device_predicates(self):self.assertIn('PASS fixed read-only',self.run_case('parsers'))
    def test_no_journal_or_missing_state(self):
        self.assertEqual(self.run_case('needed',self.path),'0');self.assertEqual(self.run_case('needed',self.path/'missing'),'0')
    def test_valid_metadata_is_needed_not_confirmed(self):
        self.write();self.assertEqual(self.run_case('needed',self.path),'1')
    def test_unsafe_metadata_rejected(self):
        p=self.write(bytes(239));self.assertEqual(self.run_case('needed',self.path),'-1')
        p.unlink();p=self.write();p.chmod(0o644);self.assertEqual(self.run_case('needed',self.path),'-1')
        p.chmod(0o600);os.link(p,self.path/'hardlink');self.assertEqual(self.run_case('needed',self.path),'-1')
    def test_symlink_and_directory_rejected(self):
        p=self.path/'install-pending';os.symlink(self.path/'missing',p);self.assertEqual(self.run_case('needed',self.path),'-1')
        p.unlink();p.mkdir();self.assertEqual(self.run_case('needed',self.path),'-1')
    def test_private_directory_required(self):
        self.write();self.path.chmod(0o755);self.assertEqual(self.run_case('needed',self.path),'-1')
    def test_unsupported_host_refuses_before_any_device_probe(self):
        self.write();self.assertIn('PASS unsupported host refused',self.run_case('unsupported',self.path))

if __name__=='__main__':unittest.main(verbosity=2)
