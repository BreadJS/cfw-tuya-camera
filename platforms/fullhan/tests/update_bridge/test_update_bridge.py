#!/usr/bin/env python3
"""Real planner/flock/fork/exec; metadata-only child and mocked reboot."""
import sys
from pathlib import Path
import subprocess, tempfile, os, unittest, json
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'boot_confirm'))
import test_boot_confirm as fixtures
ROOT=fixtures.ROOT

class Bridge(fixtures.Confirmation):
    # Reuse fixture creation only; do not inherit core confirmation test methods.
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-bridge-build-');b=Path(cls.build.name);cls.binary=b/'driver';cls.helper=b/'fake-helper'
        cc=os.environ.get('CC','cc');here=Path(__file__).parent;ota=ROOT/'src/ota';control=ROOT/'src/native/control'
        subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror',str(here/'fake_helper.c'),'-o',str(cls.helper)],check=True)
        objects=[]
        for source in ['monocypher.c','monocypher-ed25519.c']:
            obj=b/(source+'.o');subprocess.run([cc,'-std=c11','-O0','-c',str(ota/'vendor'/source),'-o',str(obj)],check=True);objects.append(str(obj))
        subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DCAMERA_UPDATE_INSTALL_TEST','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ota/'vendor'),str(here/'driver.c'),str(control/'update_install.c'),str(control/'update.c'),str(control/'http.c'),str(ota/'ota_apply_plan.c'),str(ota/'ota_state.c'),str(ota/'ota_core.c'),*objects,'-Wl,--gc-sections','-pthread','-o',str(cls.binary)],check=True)
        undefined=subprocess.check_output(['nm','-u',str(cls.binary)],text=True)
        for line in undefined.splitlines():
            if line.split()[-1].split('@')[0] in ('reboot','sync'):raise AssertionError('Real reboot/sync linked: '+line)
    def setUp(self):
        super().setUp();self.progress=self.path/'progress';self.progress.mkdir(mode=0o700);self.write(self.progress/'scenario',b'10')
    def execute(self,mode,exitcode=10,older=False):
        self.write(self.progress/'scenario',str(exitcode).encode())
        if older:self.write(self.state/'install-pending',b'FHCBOOT1'+bytes(24)+self.cap[:192]+bytes(16))
        r=subprocess.run([str(self.binary),mode,str(self.key),str(self.state),str(self.stage),str(self.helper),str(self.progress),str(self.map),self.cap[64:128].hex()],capture_output=True,text=True,timeout=15)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr);return r.stdout
    def test_handoff_high_fd_and_pristine_cancel(self):
        out=self.execute('high_fd');self.assertTrue((self.progress/'receipt').exists());self.assertIn('cancel=200',out);self.assertFalse((self.state/'install-pending').exists());self.assertEqual(self.sequence(),'1\n')
    def test_fork_failure_never_executes(self):
        self.execute('fork_fail');self.assertFalse((self.progress/'receipt').exists())
    def test_queued_factory_reset_blocks_install_plan_and_upload(self):
        self.execute('reset_pending');self.assertFalse((self.progress/'receipt').exists())
    def test_thread_failure_gate_never_executes(self):
        self.execute('thread_fail');self.assertFalse((self.progress/'receipt').exists())
    def test_stale_sequence_never_executes(self):
        self.execute('stale_sequence');self.assertFalse((self.progress/'receipt').exists())
    def test_stale_digest_never_executes(self):
        self.execute('stale_digest');self.assertFalse((self.progress/'receipt').exists())
    def test_success_mock_reboot_and_grace(self):
        out=self.execute('success',0);receipt=int((self.progress/'receipt').read_text());stamp=int(out.split('reboot_ns=')[1].split()[0]);self.assertGreaterEqual(stamp-receipt,1_950_000_000);self.assertIn('grace_busy=1',out);self.assertEqual(self.sequence(),'1\n');self.assertTrue((self.state/'install-pending').exists())
    def test_mismatched_journal_cannot_cancel(self):
        self.assertIn('cancel=409',self.execute('mismatch','tamper'));self.assertTrue((self.state/'install-pending').exists())
    def test_dirty_failure_cannot_cancel(self):
        self.assertIn('cancel=409',self.execute('dirty',11));self.assertTrue((self.state/'install-pending').exists())
    def test_older_journal_cannot_cancel(self):
        self.assertIn('cancel=409',self.execute('older',10,older=True));self.assertTrue((self.state/'install-pending').exists())
    def test_pristine_retry_remains_cancelable(self):self.assertIn('cancel=200',self.execute('retry_pristine'))
    def test_dirty_then_pristine_retry_cannot_cancel(self):self.assertIn('cancel=409',self.execute('retry_dirty',11))
    def test_progress_is_escaped_string(self):
        for value in [b'{"phase":"readback"}',b'} , "installation_phase":"written_reboot_pending", "x":{',b'"\\\n\t',b'{\x00}',b'a'*1025]:
            with self.subTest(value=value[:60]):
                self.write(self.progress/'install-progress.json',value);result=json.loads(self.execute('progress'));self.assertEqual(result['installation_phase'],'idle');self.assertIsInstance(result['installation_progress'],str)
                self.assertEqual(result['installation_progress'],'' if b'\x00' in value or len(value)>1024 else value.decode())

# Fixture inheritance is deliberate; strip unrelated inherited core tests.
for name in dir(fixtures.Confirmation):
    if name.startswith('test_') and name not in Bridge.__dict__:setattr(Bridge,name,None)
if __name__=='__main__':unittest.main(verbosity=2)
