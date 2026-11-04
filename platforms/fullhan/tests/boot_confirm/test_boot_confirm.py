#!/usr/bin/env python3
"""Host fixtures only: signed installed-byte proof and torn commit recovery."""
import hashlib
import os
from pathlib import Path
import fcntl
import struct
import subprocess
import tempfile
import unittest
import zlib
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT=Path(__file__).resolve().parents[2]
KEY=Ed25519PrivateKey.from_private_bytes(bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60'))
INSTALL_ID='01'*16
BOOT_ID='02'*16

def kernel():
    body=b'Synthetic fixture, not a bootable kernel.'
    header=bytearray(64);struct.pack_into('>I',header,0,0x27051956)
    struct.pack_into('>III',header,12,len(body),0xa0008000,0xa0008000)
    struct.pack_into('>I',header,24,zlib.crc32(body));header[28:31]=bytes([5,2,2])
    struct.pack_into('>I',header,4,zlib.crc32(header));return bytes(header)+body

def rootfs():
    b=bytearray(96);b[:4]=b'hsqs';struct.pack_into('<H',b,20,4)
    struct.pack_into('<HH',b,28,4,0);struct.pack_into('<Q',b,40,len(b));return bytes(b)

def envelope(records,sequence=2):
    vendor=b'Fuhan8626V100'.ljust(20,b'\0')+struct.pack('>I',0xabcd1234)+bytes(8)
    payload=vendor+b''.join(struct.pack('>III',kind,len(data),0)+data for kind,data in records)
    h=b'FHCAMO1\0'+struct.pack('<II',1,192)+b'fh8626v100-mis2006-8m-v1'.ljust(32,b'\0')+struct.pack('<QQ',sequence,len(payload))+hashlib.blake2b(payload).digest()
    return h+KEY.sign(h)+payload

class Confirmation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-boot-confirm-build-')
        cls.binary=Path(cls.build.name)/'driver'
        cc=os.environ.get('CC','cc');sources=ROOT/'src/ota'
        # Crypto is unchanged vendored code; instrument our code and parser.
        objects=[]
        for source in ['monocypher.c','monocypher-ed25519.c']:
            obj=Path(cls.build.name)/(source+'.o');subprocess.run([cc,'-std=c11','-O0','-c',str(sources/'vendor'/source),'-o',str(obj)],check=True);objects.append(str(obj))
        subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(sources/'vendor'),str(Path(__file__).with_name('driver.c')),str(sources/'boot_confirm.c'),str(sources/'ota_apply_plan.c'),str(sources/'ota_state.c'),str(sources/'ota_core.c'),*objects,'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.build.cleanup()

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='camera-boot-confirm-case-');self.path=Path(self.temp.name)
        self.state=self.path/'state';self.stage=self.path/'stage';self.state.mkdir(mode=0o700);self.stage.mkdir(mode=0o700)
        self.key=self.path/'key';self.write(self.key,KEY.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw))
        self.images={2:kernel(),3:rootfs()};self.cap=envelope(list(self.images.items()))
        self.write(self.state/'accepted',b'1\n');self.write(self.state/'pending',self.cap[:192]);self.write(self.state/'lock',b'');self.write(self.stage/'pending.cap',self.cap)
        self.map=self.path/'mtd';self.write(self.map,b'dev:    size   erasesize  name\n'+b''.join(f'mtd{i}: {size:08x} 00010000 "{name}"\n'.encode() for i,(size,name) in enumerate([(0x40000,'bootstrap'),(0x10000,'uboot-env'),(0x40000,'uboot'),(0x1d0000,'kernel'),(0x550000,'rootfs'),(0x50000,'config')])))
        for kind,data in self.images.items():self.image_write(kind,data)

    def tearDown(self):self.temp.cleanup()
    def write(self,path,data):path.write_bytes(data);path.chmod(0o600)
    def image_write(self,kind,data):self.write(self.path/f'image{kind}',bytes(data).ljust(0x1d0000 if kind==2 else 0x550000,b'\xff'))
    def run_case(self,args,good=True):
        r=subprocess.run([str(self.binary),*map(str,args)],capture_output=True,text=True,timeout=15)
        self.assertEqual(r.returncode==0,good,r.stdout+r.stderr);return r.stdout
    def prepare(self,boot=INSTALL_ID,fault='',good=True):return self.run_case(['prepare',self.key,self.state,self.stage,self.map,boot,fault],good)
    def confirm(self,boot=BOOT_ID,health=1,fault='',good=True):return self.run_case(['confirm',self.key,self.state,self.path/'image2',self.path/'image3',boot,health,fault],good)
    def sequence(self):return (self.state/'accepted').read_text()
    def journal(self):return self.state/'install-pending'

    def test_staging_alone_never_confirms(self):
        self.assertIn('rc=0',self.confirm());self.assertEqual(self.sequence(),'1\n');self.assertTrue((self.state/'pending').exists())
    def test_fresh_boot_actual_images_and_health(self):
        self.prepare();self.assertEqual(self.journal().stat().st_size,240)
        self.assertEqual(self.sequence(),'1\n');self.confirm(boot=INSTALL_ID,good=False);self.confirm(health=0,good=False);self.confirm(fault='sd_boot',good=False)
        self.assertEqual(self.sequence(),'1\n');self.assertIn('rc=1',self.confirm(fault='short_reads'))
        self.assertEqual(self.sequence(),'2\n');self.assertFalse(self.journal().exists());self.assertFalse((self.state/'pending').exists())
        self.assertIn('rc=0',self.confirm())
    def test_actual_installed_corruption_rejected(self):
        self.prepare();b=bytearray(self.images[2]);b[-1]^=1;self.image_write(2,b)
        self.confirm(good=False);self.assertEqual(self.sequence(),'1\n');self.assertTrue(self.journal().exists())
    def test_valid_rootfs_header_but_wrong_signed_bytes_rejected(self):
        self.prepare();b=bytearray(self.images[3]);b[88]^=1;self.image_write(3,b)
        self.confirm(good=False);self.assertEqual(self.sequence(),'1\n')
    def test_incomplete_erased_tail_rejected(self):
        self.prepare();p=self.path/'image3';b=bytearray(p.read_bytes());b[-1]=0;self.image_write(3,b)
        self.confirm(good=False);self.assertEqual(self.sequence(),'1\n')
    def test_journal_tampering(self):
        self.prepare();original=self.journal().read_bytes()
        for offset in [0,8,12,32+48,32+128,224,228,232,236]:
            with self.subTest(offset=offset):
                b=bytearray(original);b[offset]^=1;self.write(self.journal(),b);self.confirm(good=False);self.assertEqual(self.sequence(),'1\n')
        self.write(self.journal(),original);self.confirm()
    def test_pending_mismatch_and_missing(self):
        self.prepare();self.write(self.state/'pending',bytes(192));self.confirm(good=False)
        (self.state/'pending').unlink();self.confirm(good=False);self.assertEqual(self.sequence(),'1\n')
    def test_truncated_journal_and_link_rejected(self):
        self.prepare();original=self.journal().read_bytes();self.write(self.journal(),original[:-1]);self.confirm(good=False)
        self.journal().unlink();os.symlink(self.stage/'pending.cap',self.journal());self.confirm(good=False);self.assertEqual(self.sequence(),'1\n')
    def test_prepare_reauthenticates(self):
        b=bytearray(self.cap);b[-1]^=1;self.write(self.stage/'pending.cap',b);self.prepare(good=False);self.assertFalse(self.journal().exists())
    def test_reapply_refreshes_installer_boot_id(self):
        self.prepare();self.prepare(boot=BOOT_ID);self.confirm(boot=BOOT_ID,good=False);self.confirm(boot='03'*16)
    def test_accepted_not_rolled_back(self):
        self.prepare();self.write(self.state/'accepted',b'3\n');self.confirm(good=False);self.assertEqual(self.sequence(),'3\n')
    def test_state_change_during_installed_readback(self):
        self.prepare();self.confirm(fault='mutate_accepted',good=False);self.assertEqual(self.sequence(),'9\n')
    def test_confirmation_crash_boundaries_recover(self):
        for point in ['accepted_file_synced','accepted_renamed','accepted_directory_synced','pending_unlinked','journal_unlinked']:
            with self.subTest(point=point):
                self.write(self.state/'accepted',b'1\n');self.write(self.state/'pending',self.cap[:192]);self.journal().unlink(missing_ok=True)
                self.prepare();self.confirm(fault=point,good=False);self.assertIn(self.sequence(),['1\n','2\n'])
                self.confirm();self.assertEqual(self.sequence(),'2\n');self.assertFalse(self.journal().exists());self.assertFalse((self.state/'pending').exists())
    def test_journal_crash_boundaries_never_advance_accepted(self):
        for point in ['journal_file_synced','journal_renamed','journal_directory_synced']:
            with self.subTest(point=point):
                self.journal().unlink(missing_ok=True);self.prepare(fault=point,good=False);self.assertEqual(self.sequence(),'1\n')
                self.prepare();self.confirm();self.assertEqual(self.sequence(),'2\n')
                self.write(self.state/'accepted',b'1\n');self.write(self.state/'pending',self.cap[:192])
    def test_rootfs_only(self):
        self.cap=envelope([(3,self.images[3])]);self.write(self.stage/'pending.cap',self.cap);self.write(self.state/'pending',self.cap[:192]);self.prepare();self.confirm();self.assertEqual(self.sequence(),'2\n')
    def test_orphan_temporaries_cleaned_without_touching_foreign_files(self):
        orphan=self.state/'boot-confirm-100-1.tmp';foreign=self.state/'unrelated.tmp'
        self.write(orphan,b'interrupted');self.write(foreign,b'keep');self.prepare()
        self.assertFalse(orphan.exists());self.assertEqual(foreign.read_bytes(),b'keep')
        self.write(orphan,b'interrupted');self.confirm();self.assertFalse(orphan.exists());self.assertTrue(foreign.exists())
    def test_unsafe_orphan_and_accepted_file_fail_closed(self):
        orphan=self.state/'boot-confirm-100-1.tmp';os.symlink(self.state/'accepted',orphan)
        self.prepare(good=False);self.assertTrue(orphan.is_symlink());self.assertEqual(self.sequence(),'1\n')
        orphan.unlink();self.prepare();(self.state/'accepted').chmod(0o644);self.confirm(good=False)
        self.assertEqual(self.sequence(),'1\n')
    def test_exclusive_lock_required(self):
        self.prepare()
        with (self.state/'lock').open('rb') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);self.confirm(good=False)
        self.assertEqual(self.sequence(),'1\n');self.confirm()
    def test_recovery_precedes_legacy_state_open(self):
        self.prepare();self.confirm(fault='accepted_renamed',good=False)
        (self.stage/'pending.cap').unlink() # fresh RAM staging after reboot
        self.run_case(['state',self.key,self.state,self.stage],good=False)
        self.confirm();text=self.run_case(['state',self.key,self.state,self.stage])
        self.assertIn('accepted=2 pending=0',text)
    def test_already_accepted_cleanup_survives_later_media_fault(self):
        self.prepare();self.confirm(fault='accepted_directory_synced',good=False)
        self.assertEqual(self.sequence(),'2\n');text=self.confirm(health=0)
        self.assertIn('already=1 committed=1 cleanup=1',text)
        self.assertFalse(self.journal().exists())

if __name__=='__main__':unittest.main(verbosity=2)
