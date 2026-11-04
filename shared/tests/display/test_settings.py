#!/usr/bin/env python3
import datetime, os, pathlib, struct, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
class Display(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-display-build-');cls.binary=pathlib.Path(cls.build.name)/'driver'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(pathlib.Path(__file__).with_name('driver.c')),str(ROOT/'src/display/settings.c'),'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):self.temp=tempfile.TemporaryDirectory(prefix='camera-display-case-');self.path=pathlib.Path(self.temp.name)
    def tearDown(self):self.temp.cleanup()
    def run_case(self,op,*args,good=True):
        cmd=[str(self.binary),op]
        if op!='clock':cmd.append(str(self.path))
        result=subprocess.run(cmd+list(map(str,args)),capture_output=True,text=True,timeout=5);self.assertEqual(result.returncode==0,good,result.stdout+result.stderr);return result.stdout.rstrip('\n')
    def save(self,name=b'Camera',enabled=0,tz=0,good=True):return self.run_case('save',name.hex(),enabled,tz,good=good)
    def write(self,b):p=self.path/'display.conf';p.write_bytes(b);p.chmod(0o600)
    def clock(self,iso,tz=0,good=True):
        epoch=int(datetime.datetime.fromisoformat(iso).replace(tzinfo=datetime.timezone.utc).timestamp());return self.run_case('clock',epoch,tz,good=good)
    def test_defaults_absent(self):self.assertEqual(self.run_case('load'),'0 0 Camera');self.assertFalse((self.path/'display.conf').exists())
    def test_save_reload_wire_canonical_and_replace(self):
        self.save('Voordeur ☀'.encode(),1,1);self.assertEqual(self.run_case('load'),'1 1 Voordeur ☀');p=self.path/'display.conf';b=p.read_bytes();self.assertEqual(len(b),80);self.assertEqual(b[:16],b'CAMDSP1\0'+struct.pack('<II',1,1));self.assertEqual(p.stat().st_mode&0o777,0o600)
        self.save(b'New');self.assertEqual(self.run_case('load'),'0 0 New');self.assertFalse(list(self.path.glob('display.tmp.*')))
    def test_utf8_valid_boundaries(self):
        for name in [b'a',b'x'*63,'é🙂漢字'.encode(),b' leading space ',b'\xf4\x8f\xbf\xbf']:
            with self.subTest(name=name):self.save(name);self.assertTrue(self.run_case('load').endswith(name.decode()))
    def test_invalid_utf8_controls_and_bounds(self):
        for name in [b'',b'a'*64,b'a\nb',b'\x01',b'\x7f',b'\x80',b'\xc0\x80',b'\xc1\xbf',b'\xc2',b'\xe0\x80\x80',b'\xed\xa0\x80',b'\xf0\x80\x80\x80',b'\xf4\x90\x80\x80',b'\xf5\x80\x80\x80',b'\xff']:
            with self.subTest(name=name):self.save(name,good=False)
        self.assertFalse((self.path/'display.conf').exists())
    def test_invalid_modes(self):
        self.save(enabled=2,good=False);self.save(tz=2,good=False);self.run_case('clock',0,2,good=False)
    def test_wire_corruption_preserves_output(self):
        self.save();original=(self.path/'display.conf').read_bytes()
        for b in [original[:-1],original+b'x',b'X'+original[1:],original[:8]+struct.pack('<I',2)+original[12:],original[:12]+struct.pack('<I',2)+original[16:],original[:16]+b'x'*64,original[:16]+b'\0'*64,original[:16]+b'bad\n'+bytes(60),original[:16]+b'A\0X'+bytes(61)]:
            with self.subTest(data=b[:20]):self.write(b);self.run_case('load',good=False)
    def test_symlink_and_hardlink_reject(self):
        target=self.path/'factory';target.write_bytes(b'untouched');p=self.path/'display.conf';p.symlink_to(target);self.run_case('load',good=False);self.save(good=False);self.assertEqual(target.read_bytes(),b'untouched');p.unlink();self.save();os.link(p,self.path/'alias');self.run_case('load',good=False);self.save(good=False)
    def test_permissions_and_private_directory(self):
        self.save();p=self.path/'display.conf';p.chmod(0o644);self.run_case('load',good=False);self.save(good=False);p.chmod(0o600);self.path.chmod(0o755);self.run_case('load',good=False);self.save(good=False)
    def test_amsterdam_2026_dst_boundaries(self):
        cases={'2026-03-29T00:59:59':'2026-03-29 Sun 01:59:59','2026-03-29T01:00:00':'2026-03-29 Sun 03:00:00','2026-10-25T00:59:59':'2026-10-25 Sun 02:59:59','2026-10-25T01:00:00':'2026-10-25 Sun 02:00:00','2026-10-29T12:34:56':'2026-10-29 Thu 13:34:56','2026-06-30T23:30:00':'2026-07-01 Wed 01:30:00','2026-12-31T23:59:59':'2027-01-01 Fri 00:59:59'}
        for iso,expected in cases.items():
            with self.subTest(iso=iso):self.assertEqual(self.clock(iso),expected);self.assertEqual(len(expected),23)
    def test_utc_and_global_timezone_independence(self):
        self.assertEqual(self.clock('2026-03-29T01:00:00',1),'2026-03-29 Sun 01:00:00');self.assertEqual(self.clock('2026-10-29T12:34:56',1),'2026-10-29 Thu 12:34:56');self.assertEqual(self.clock('2024-02-29T23:59:59',1),'2024-02-29 Thu 23:59:59')
if __name__=='__main__':unittest.main(verbosity=2)
