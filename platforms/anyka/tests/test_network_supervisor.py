#!/usr/bin/env python3
"""Exercise only radio-operation functions with host mocks; never touches WLAN."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'rootfs-overlay/usr/libexec/camera/network-supervisor'


class Reload(unittest.TestCase):
    def test_pending_reset_blocks_old_wifi_until_cleared(self):
        source=SCRIPT.read_text()
        block=source[source.index('# The application retries an interrupted reset'):source.index('\nmode=station\n')]
        write=source[source.index('write() {'):source.index('stop_pid() {')]
        with tempfile.TemporaryDirectory(prefix='network-reset-') as tmp:
            temp=Path(tmp);config=temp/'config';config.mkdir();marker=config/'reset.pending'
            marker.write_text('pending\n');(temp/'reset-check').write_text(write+block)
            setup='R=$1; CAMERA_CONFIG_DIR="$R/config"; count=0; '
            result=subprocess.run(['sh','-c',setup+'''
                sleep() { count=$((count+1)); if [ "$count" = 3 ]; then rm "$CAMERA_CONFIG_DIR/reset.pending"; fi; }
                . "$R/reset-check"
                echo reset-finished-before-radio-start
            ''','test',tmp],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('reset-finished-before-radio-start',result.stdout)
            marker.write_text('pending\n')
            result=subprocess.run(['sh','-c',setup+'''
                sleep() { :; }
                . "$R/reset-check"
                echo must-not-reach-radio-start
            ''','test',tmp],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,1)
            self.assertNotIn('must-not-reach',result.stdout)
            self.assertEqual((temp/'mode').read_text(),'error')
            self.assertTrue(marker.exists())

    def test_ap_readiness_survives_hostapd_five_second_country_update(self):
        source=SCRIPT.read_text()
        functions=source[source.index('ap_status() {'):source.index('\nmode=station\n')]
        write=source[source.index('write() {'):source.index('stop_pid() {')]
        with tempfile.TemporaryDirectory(prefix='ap-country-delay-') as tmp:
            temp=Path(tmp);(temp/'functions').write_text(write+functions)
            (temp/'hostapd').write_text('#!/bin/sh\nexec /bin/sleep 1\n')
            (temp/'hostapd_cli').write_text('''#!/bin/sh
count=$(cat "$R/cli-count" 2>/dev/null || echo 0)
count=$((count+1))
echo "$count" >"$R/cli-count"
if [ "$count" -le 6 ]; then echo state=COUNTRY_UPDATE; else echo state=ENABLED; fi
''')
            (temp/'hostapd').chmod(0o700);(temp/'hostapd_cli').chmod(0o700)
            result=subprocess.run(['sh','-c','''
                R=$1; B=$1; export R
                CAMERA_CONFIG_DIR="$R/config"
                . "$R/functions"
                stop_network() { :; }
                reload_radio() { :; }
                ifconfig() { :; }
                sleep() { :; }
                udhcpd() { exec /bin/sleep 1; }
                cat() {
                    if [ "${1:-}" = /sys/class/net/wlan0/address ]; then
                        echo 00:11:22:5b:30:db
                    else /bin/cat "$@"; fi
                }
                access_point
                rc=$?
                [ ! -f "$R/hostapd.pid" ] || kill "$(cat "$R/hostapd.pid")" 2>/dev/null
                [ ! -f "$R/udhcpd.pid" ] || kill "$(cat "$R/udhcpd.pid")" 2>/dev/null
                exit "$rc"
            ''','test',tmp],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual((temp/'cli-count').read_text().strip(),'7')
            self.assertEqual((temp/'mode').read_text(),'ap')
            self.assertIn('AP_STATUS uptime=',(temp/'ap-readiness.log').read_text())

    def run_shell(self, code):
        source=SCRIPT.read_text()
        functions=source[source.index('module_operation() {'):source.index('station() {')]
        with tempfile.TemporaryDirectory(prefix='network-functions-') as tmp:
            temp=Path(tmp);(temp/'functions').write_text(functions)
            return subprocess.run(['sh','-c','R="$1"; module_uncertain=0; . "$R/functions"; '+code,'test',tmp],
                                  capture_output=True,text=True,timeout=5)

    def test_reload_is_opt_in(self):
        result=self.run_shell('module_operation() { exit 42; }; reload_radio; echo disabled-safe')
        self.assertEqual(result.returncode,0)
        self.assertIn('disabled-safe',result.stdout)

    def test_vendor_order_and_failure_stops_next_operation(self):
        result=self.run_shell('''
            CAMERA_NETWORK_RELOAD_DRIVER=1
            grep() { return 0; }
            sleep() { :; }
            module_operation() { printf '%s\n' "$1"; return 0; }
            reload_radio
        ''')
        self.assertEqual(result.returncode,0)
        self.assertEqual(result.stdout.splitlines(),[
            'remove-rtl8188ftv','remove-ak_hcd','remove-usbcore','remove-usb-common','remove-cfg80211',
            'insert-cfg80211','insert-usb-common','insert-usbcore','insert-ak_hcd','insert-rtl8188ftv'])
        result=self.run_shell('''
            CAMERA_NETWORK_RELOAD_DRIVER=1
            grep() { return 0; }
            module_operation() { echo "$1"; [ "$1" != remove-ak_hcd ]; }
            reload_radio
        ''')
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(result.stdout.splitlines(),['remove-rtl8188ftv','remove-ak_hcd'])

    def test_operation_returns_real_exit_status(self):
        result=self.run_shell('module_operation test /bin/sh -c "exit 7"; status=$?; cat "$R/module.log"; exit "$status"')
        self.assertEqual(result.returncode,7)
        self.assertIn('done=test rc=7',result.stdout)

    def test_deadline_refuses_and_logs_unconfirmed_kernel_completion(self):
        result=self.run_shell('''
            sleep() { /bin/sleep .01; }
            module_operation slow /bin/sleep 20
            status=$?
            cat "$R/module.log"
            exit "$status"
        ''')
        self.assertEqual(result.returncode,1)
        self.assertIn('timeout=slow',result.stdout)
        self.assertIn('kernel_task_may_remain=yes',result.stdout)


if __name__=='__main__':unittest.main()
