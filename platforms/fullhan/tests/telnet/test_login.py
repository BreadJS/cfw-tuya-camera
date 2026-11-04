#!/usr/bin/env python3
"""Real PTY/password-echo tests; test binary never executes a shell."""
from pathlib import Path
import os,pty,select,struct,subprocess,tempfile,time,unittest
ROOT=Path(__file__).resolve().parents[2]
class Login(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-telnet-build-');cls.binary=Path(cls.build.name)/'login'
        subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
                        '-DCAMERA_TELNET_LOGIN_TEST','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        str(ROOT/'src/telnet/login.c'),str(ROOT/'src/native/control/credentials.c'),
                        '-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='camera-telnet-test-');self.path=Path(self.temp.name)
        self.config=self.path/'config';self.config.mkdir(mode=0o700)
        self.initial=self.path/'bootstrap.auth';self.initial.write_text('admin:fixture-default\n');self.initial.chmod(0o600)
        self.process=None;self.master=None;self.captured=b''
    def tearDown(self):
        if self.process and self.process.poll() is None:self.process.terminate();self.process.wait(timeout=3)
        if self.master is not None:os.close(self.master)
        self.temp.cleanup()
    def credentials(self,web='owner:fixture-password',stream='stream:other-password'):
        data=b'CAMAUTH1'+web.encode().ljust(256,b'\0')+stream.encode().ljust(256,b'\0')
        f=self.config/'credentials.conf';f.write_bytes(data);f.chmod(0o600);return f
    def start(self):
        self.master,slave=pty.openpty()
        self.process=subprocess.Popen([str(self.binary),str(self.config),str(self.initial)],stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
        os.close(slave)
    def until(self,text):
        data=b'';end=time.monotonic()+5
        while text not in data and time.monotonic()<end:
            if select.select([self.master],[],[],.1)[0]:
                try:part=os.read(self.master,8192)
                except OSError:break
                if not part:break
                data+=part;self.captured+=part
        self.assertIn(text,data,data.decode(errors='replace'));return data
    def authenticate(self,user,password,expected=b'AUTH_OK'):
        self.until(b'Login: ');os.write(self.master,user.encode()+b'\n');self.until(b'Password: ')
        os.write(self.master,password.encode()+b'\n');data=self.until(expected)
        self.assertNotIn(password.encode(),data);return data
    def test_default_login_and_password_not_echoed(self):
        self.start();self.authenticate('admin','fixture-default');self.assertEqual(self.process.wait(timeout=3),0)
    def test_saved_web_login(self):
        self.credentials();self.start();self.authenticate('owner','fixture-password')
    def test_wrong_password_then_correct(self):
        self.credentials();self.start();self.authenticate('owner','wrong-password',b'Login incorrect.')
        self.authenticate('owner','fixture-password')
    def test_stream_password_cannot_open_root_shell(self):
        self.credentials();self.start();self.authenticate('stream','other-password',b'Login incorrect.')
        self.assertNotIn(b'AUTH_OK',self.captured)
    def test_changed_web_password_reloaded_after_failed_attempt(self):
        self.credentials();self.start();self.authenticate('owner','wrong-password',b'Login incorrect.')
        self.credentials(web='owner:changed-password');self.authenticate('owner','changed-password')
    def test_corrupt_credentials_dont_fall_back_to_default(self):
        f=self.credentials();f.write_bytes(b'broken');self.start();self.authenticate('admin','fixture-default',b'Login unavailable.')
        self.assertNotIn(b'AUTH_OK',self.captured)
    def test_symlink_credentials_rejected(self):
        (self.config/'credentials.conf').symlink_to(self.initial)
        self.start();self.authenticate('admin','fixture-default',b'Login unavailable.')
    def test_nonprivate_credentials_rejected(self):
        self.credentials().chmod(0o644);self.start();self.authenticate('owner','fixture-password',b'Login unavailable.')
    def test_nonprivate_bootstrap_rejected(self):
        self.initial.chmod(0o644);self.start();self.authenticate('admin','fixture-default',b'Login unavailable.')
if __name__=='__main__':unittest.main()
