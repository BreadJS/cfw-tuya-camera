#!/usr/bin/env python3
"""Host-only reset transactions; no real camera configuration is touched."""
import fcntl, os, pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[2]
FILES=['wpa_supplicant.conf','credentials.conf','settings.conf','recovery.conf','illumination.conf','indicator.conf','display.conf','rotation.conf','picture.conf']
TEMPS=['credentials.tmp.123','recovery.conf.tmp.123','settings.tmp.123.5','illumination.tmp.123.6','indicator.tmp.123.7','display.tmp.123.8','rotation.tmp.123.9','picture.tmp.123.10','.wifi-123-8.tmp']
class Reset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-reset-build-');cls.binary=pathlib.Path(cls.build.name)/'driver';cls.cli=pathlib.Path(cls.build.name)/'config-reset'
        cc=os.environ.get('CC','cc');source=ROOT/'src/reset/config_reset.c'
        base=[cc,'-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(base+[str(pathlib.Path(__file__).with_name('driver.c')),str(source),'-o',str(cls.binary)],check=True)
        subprocess.run(base+['-DCAMERA_CONFIG_RESET_MAIN',str(source),'-o',str(cls.cli)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='camera-reset-case-');self.path=pathlib.Path(self.temp.name);self.state=self.path/'update-state';self.state.mkdir(mode=0o700)
        self.preserved={'update.pub':b'key32', 'factory-calibration':b'FACTORY','.wifi-factory.tmp':b'factory','credentials.tmp.factory':b'factory','settings.tmp.1.2.extra':b'factory','recovery.conf.tmp.1.extra':b'factory'}
        self.ota={'accepted':b'7\n','pending':b'signed header','lock':b''}
        for name,data in self.preserved.items():self.write(self.path/name,data)
        for name,data in self.ota.items():self.write(self.state/name,data)
        for name in FILES+TEMPS:self.write(self.path/name,b'old secret or malformed content\x00')
    def tearDown(self):self.temp.cleanup()
    def write(self,p,data):p.write_bytes(data);p.chmod(0o600)
    def run_reset(self,action,point='',borrowed='local',busy=0,good=True):
        result=subprocess.run([str(self.binary),action,str(self.path),point,borrowed,str(busy)],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode==0,good,result.stdout+result.stderr);return result.stdout
    def check_preserved(self):
        for name,data in self.preserved.items():self.assertEqual((self.path/name).read_bytes(),data)
        for name,data in self.ota.items():self.assertEqual((self.state/name).read_bytes(),data)
    def check_reset(self):
        for name in FILES+TEMPS:self.assertFalse((self.path/name).exists(),name)
        self.assertFalse((self.path/'reset-request').exists());self.check_preserved()
    def test_request_only_queues_and_apply_preserves_ota(self):
        self.run_reset('request',borrowed='borrowed');self.run_reset('request');self.assertTrue((self.path/'credentials.conf').exists());self.assertIn('rc=1',self.run_reset('apply'));self.check_reset();self.assertIn('rc=0',self.run_reset('apply'))
    def test_busy_does_not_publish(self):
        self.run_reset('request',busy=1,good=False);self.assertFalse((self.path/'reset-request').exists());self.check_preserved()
    def test_dirty_install_blocks_both_paths(self):
        self.run_reset('request');self.write(self.state/'install-pending',b'dirty');self.run_reset('request',good=False);self.run_reset('apply',good=False);self.assertEqual((self.state/'install-pending').read_bytes(),b'dirty');self.assertTrue((self.path/'credentials.conf').exists());self.check_preserved()
    def test_distinct_lock_holder_blocks_reset(self):
        with (self.state/'lock').open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);self.run_reset('request',good=False)
        self.run_reset('request')
        with (self.state/'lock').open('r+') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);self.run_reset('apply',good=False)
        self.run_reset('apply');self.check_reset()
    def test_wrong_borrowed_lock_rejected(self):self.run_reset('request',borrowed='wrong',good=False);self.check_preserved()
    def test_request_crash_boundaries(self):
        for point in ['marker_file_synced','marker_renamed','marker_directory_synced']:
            with self.subTest(point=point):
                self.run_reset('request',point,good=False);self.assertTrue((self.path/'credentials.conf').exists());self.run_reset('request');self.run_reset('apply');self.check_reset()
                for name in FILES+TEMPS:self.write(self.path/name,b'old')
    def test_apply_crash_boundaries_retry(self):
        for point in FILES+['temporaries_removed','config_directory_synced','marker_unlinked','completed_directory_synced']:
            with self.subTest(point=point):
                for name in FILES+TEMPS:self.write(self.path/name,b'old')
                self.run_reset('request');self.run_reset('apply',point,good=False);self.run_reset('apply');self.check_reset()
    def test_symlink_names_unlinked_without_following(self):
        for name in ['credentials.conf','settings.tmp.123.5']:
            (self.path/name).unlink();(self.path/name).symlink_to(self.path/'factory-calibration')
        self.run_reset('request');self.run_reset('apply');self.check_reset()
    def test_directory_in_own_slot_fails_and_retries(self):
        p=self.path/'credentials.conf';p.unlink();p.mkdir();self.run_reset('request');self.run_reset('apply',good=False);self.assertTrue((self.path/'reset-request').exists());p.rmdir();self.run_reset('apply');self.check_reset()
    def test_marker_corruption_and_symlink_fail_closed(self):
        p=self.path/'reset-request'
        for data in [b'',b'partial',b'CAMERA_CONFIG_RESET_V1\n\x00']:
            self.write(p,data);self.run_reset('request',good=False);self.run_reset('apply',good=False);self.assertTrue((self.path/'credentials.conf').exists());p.unlink()
        p.symlink_to(self.path/'factory-calibration');self.run_reset('request',good=False);self.run_reset('apply',good=False);self.check_preserved()
    def test_private_directory_and_state_required(self):
        self.path.chmod(0o755);self.run_reset('request',good=False);self.path.chmod(0o700)
        self.state.chmod(0o755);self.run_reset('request',good=False);self.state.chmod(0o700)
        (self.state/'lock').chmod(0o644);self.run_reset('request',good=False)
    def test_missing_state_can_initialize_without_baseline(self):
        for p in self.state.iterdir():p.unlink()
        self.state.rmdir();self.run_reset('request');self.assertFalse((self.state/'accepted').exists());self.run_reset('apply');self.assertFalse((self.state/'accepted').exists());self.assertTrue((self.path/'update.pub').exists())
    def test_marker_hardlink_and_public_mode_rejected(self):
        self.run_reset('request');marker=self.path/'reset-request';os.link(marker,self.path/'marker-alias');self.run_reset('apply',good=False);(self.path/'marker-alias').unlink();marker.chmod(0o644);self.run_reset('apply',good=False);marker.chmod(0o600);self.run_reset('apply');self.check_reset()
    def test_ota_directory_symlink_never_followed(self):
        moved=self.path/'factory-state';self.state.rename(moved);self.state.symlink_to(moved,target_is_directory=True);self.run_reset('request',good=False);self.assertEqual((moved/'accepted').read_bytes(),b'7\n');self.assertFalse((self.path/'reset-request').exists())
    def test_cli_refuses_unsupported_host(self):
        result=subprocess.run([str(self.cli),'--apply'],capture_output=True,text=True,timeout=5);self.assertEqual(result.returncode,1);self.assertIn('production NOR/JFFS2',result.stderr);self.check_preserved()
if __name__=='__main__':unittest.main(verbosity=2)
