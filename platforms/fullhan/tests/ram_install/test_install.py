#!/usr/bin/env python3
"""Installer orchestration tests, generated capsules and memory/syscall fakes only."""
import json,os,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/support'))
from fixtures import ApplyFixtures, kernel, rootfs

class InstallTests(unittest.TestCase):
    setUp=ApplyFixtures.setUp
    tearDown=ApplyFixtures.tearDown
    fixture=ApplyFixtures.fixture
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='camera-ram-install-test-')
        cls.driver=Path(cls.build.name)/'driver'
        src=ROOT/'src/ota';vendor=src/'vendor'
        arguments=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DCAMERA_RAM_INSTALL_TEST','-DCAMERA_WATCHDOG_PROBE_LIBRARY','-I'+str(vendor)]
        if os.environ.get('SANITIZE')=='1':arguments+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        arguments+=[str(ROOT/'tests/ram_install/driver.c'),*[str(src/x) for x in ['ram_install.c','watchdog_clock_probe.c','boot_confirm.c','ota_linux_mtd.c','ota_apply_plan.c','ota_core.c','vendor/monocypher.c','vendor/monocypher-ed25519.c']],'-o',str(cls.driver)]
        subprocess.run(arguments,check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def run_case(self,scenario):
        with tempfile.TemporaryDirectory(prefix='camera-install-report-',dir='/dev/shm') as temp:
            progress=Path(temp);progress.chmod(0o700)
            if scenario=='progress_symlink':(progress/'install-progress.new').symlink_to('/dev/null')
            run=subprocess.run([str(self.driver),str(self.pub),str(self.state),str(self.stage),str(self.map),scenario,str(progress)],capture_output=True,text=True,timeout=40)
            self.assertEqual(run.returncode,0,run.stderr)
            result=json.loads(run.stdout)
            for key in ['leaked','protected_changed','forbidden','bad_io']:self.assertEqual(result[key],0,(scenario,key,run.stdout,run.stderr))
            self.assertEqual((self.state/'accepted').read_bytes(),b'1\n')
            self.assertEqual((self.state/'pending').read_bytes(),self.capsule[:192])
            self.assertEqual((self.stage/'pending.cap').read_bytes(),self.capsule)
            journal=(self.state/'install-pending').exists()
            self.assertEqual(journal,bool(result['journal_prepared']))
            if (progress/'install-progress.json').exists():
                data=json.loads((progress/'install-progress.json').read_text());self.assertFalse(data['accepted_advanced']);self.assertFalse(data['applied']);self.assertFalse(data['boot_confirmed'])
                self.assertLess((progress/'install-progress.json').stat().st_size,768)
                if scenario=='progress_lost_at_erase':self.assertEqual(data['phase'],'prepared')
                else:self.assertEqual(data['phase'],'written_reboot_pending' if result['rc']==0 else 'failed')
            return result
    def test_complete_and_single_partition_images(self):
        for records,count in [(None,114),([(2,kernel())],29),([(3,rootfs())],85)]:
            with self.subTest(count=count):
                self.fixture(records);r=self.run_case('success')
                self.assertEqual((r['rc'],r['journal_prepared'],r['verified_written'],r['completed']),(0,1,1,count))
                (self.state/'install-pending').unlink()
    def test_refusal_before_journal_or_device_open(self):
        for scenario in ['wrong_origin','volatile_config','watchdog_unpaused','old_NOR_mapping','lease','progress_symlink','journal_failure']:
            with self.subTest(scenario=scenario):
                r=self.run_case(scenario);self.assertNotEqual(r['rc'],0);self.assertEqual((r['journal_prepared'],r['device_opens'],r['erases']),(0,0,0))
    def test_activation_failures_preserve_recovery_journal(self):
        for scenario in ['readonly','nand','offset','size','regular','name','map','info_failure','missing_offset']:
            with self.subTest(scenario=scenario):
                r=self.run_case(scenario);self.assertNotEqual(r['rc'],0);self.assertEqual((r['journal_prepared'],r['erases'],r['verified_written']),(1,0,0))
    def test_partial_failure_never_claims_success(self):
        for scenario in ['erase_failure','erase_eintr','write_failure','write_zero','read_failure','read_zero','readback','replaced','revoke_write','revoke_sync','sync_close','sync_close_eintr']:
            with self.subTest(scenario=scenario):
                r=self.run_case(scenario);self.assertNotEqual(r['rc'],0);self.assertEqual((r['journal_prepared'],r['may_have_changed'],r['verified_written']),(1,1,0));self.assertEqual(r['erases'],1)
    def test_finalization_failure_never_claims_written_verified(self):
        for scenario in ['final_close','final_close_eintr']:
            with self.subTest(scenario=scenario):
                r=self.run_case(scenario);self.assertNotEqual(r['rc'],0);self.assertEqual((r['completed'],r['verified_written']),(114,0))
    def test_short_io_and_interrupts_finish(self):
        for scenario in ['short_io','eintr']:
            with self.subTest(scenario=scenario):
                r=self.run_case(scenario);self.assertEqual((r['rc'],r['verified_written'],r['completed']),(0,1,114))
    def test_progress_loss_cannot_authorize_first_erase(self):
        r=self.run_case('progress_lost_at_erase');self.assertNotEqual(r['rc'],0)
        self.assertEqual((r['journal_prepared'],r['may_have_changed'],r['verified_written'],r['erases']),(1,1,0,0))
    def test_actual_boot_argument_parser_excludes_recovery_and_ambiguity(self):
        valid='console=ttyS0,115200 mem=36M root=/dev/mtdblock4 rootfstype=squashfs quiet'
        cases=[(valid,True),(valid+' init=/sbin/init',True),
            (valid.replace('/dev/mtdblock4','/dev/mmcblk0p1'),False),
            (valid.replace('squashfs','vfat'),False),(valid+' root=/dev/mtdblock4',False),
            (valid+' rootfstype=squashfs',False),(valid+' camera_sd_test=0',False),
            (valid+' camera_sd_test=1',False),(valid+' camera_recovery=0',False),
            (valid+' init=/RAMINIT',False),(valid+' rdinit=/sbin/init',False),
            ('root=/dev/mtdblock4',False),('rootfstype=squashfs',False),
            ('"root=/dev/mtdblock4" rootfstype=squashfs',False),('x'*2048,False)]
        for text,expected in cases:
            with self.subTest(text=text):
                r=subprocess.run([str(self.driver),'--cmdline',text],capture_output=True,text=True,check=True)
                self.assertEqual(json.loads(r.stdout)['accepted'],expected)
    def test_cli_exit_protocol_is_conservative(self):
        for outcome,dirty,verified,status in [(0,1,1,0),(-1,0,0,10),(-1,1,0,11),(-1,1,1,11),(0,0,0,11)]:
            with self.subTest(outcome=outcome,dirty=dirty,verified=verified):
                r=subprocess.run([str(self.driver),'--exit',str(outcome),str(dirty),str(verified)],capture_output=True,text=True,check=True)
                self.assertEqual(json.loads(r.stdout)['status'],status)

if __name__=='__main__':unittest.main(verbosity=2)
