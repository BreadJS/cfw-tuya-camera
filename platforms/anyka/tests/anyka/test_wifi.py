#!/usr/bin/env python3
"""Host-only real HTTP form/file/control-socket tests, without radio operations."""
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "src/anyka/wifi.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static ssize_t body(void *s,void *p,size_t n){memcpy(p,s,n);return (ssize_t)n;}
int main(int argc,char **argv){
 if(argc!=6)return 2;
 struct anyka_network *network;int rc=anyka_network_open(&network,argv[1],argv[2]);
 if(rc){printf("{\"open_error\":%d}",rc);return 0;}
 struct control_request req={.method=argv[3],.path=argv[4],.content_type="application/x-www-form-urlencoded",.body={.opaque=argv[5],.length=strlen(argv[5]),.read=body}};
 struct control_reply reply={0};anyka_network_http(network,&req,&reply);
 printf("%u\n%s",reply.status,reply.json);return 0;
}
'''


class Wifi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='ptz-wifi-test-')
        cls.path = Path(cls.work.name)
        (cls.path / 'harness.c').write_text(HARNESS)
        cls.exe = cls.path / 'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I', str(ROOT), str(cls.path / 'harness.c'),
                        str(ROOT / 'src/anyka/wifi.c'), str(ROOT / 'src/native/control/http.c'),
                        '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='wn-', dir='/tmp')
        self.dir = Path(self.temp.name)
        self.config = self.dir / 'c'; self.runtime = self.dir / 'r'
        self.config.mkdir(mode=0o700); self.runtime.mkdir(mode=0o700)
        (self.runtime / 'wpa_ctrl').mkdir()
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.bind(str(self.runtime / 'wpa_ctrl/wlan0'))
        self.socket.settimeout(.1)
        self.stop = False
        self.completed = False
        self.commands = []
        def server():
            while not self.stop:
                try:
                    data, peer = self.socket.recvfrom(2048)
                except socket.timeout:
                    continue
                self.commands.append(data.decode())
                response = b'OK\n'
                if data == b'STATUS':
                    response = b'wpa_state=COMPLETED\n' if self.completed else b'wpa_state=DISCONNECTED\n'
                elif data == b'SCAN_RESULTS':
                    response = b'bssid / frequency / signal level / flags / ssid\naa:bb:cc:dd:ee:ff\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tTest "network"\n'
                try:
                    self.socket.sendto(response, peer)
                except FileNotFoundError:
                    pass
        self.thread = threading.Thread(target=server); self.thread.start()

    def tearDown(self):
        self.stop = True; self.thread.join(); self.socket.close(); self.temp.cleanup()

    def request(self, path='/api/wifi', method='GET', body=''):
        output = subprocess.check_output([str(self.exe), str(self.config), str(self.runtime), method, path, body], text=True)
        status, data = output.split('\n', 1)
        return int(status), json.loads(data)

    def test_candidate_is_private_and_never_shell_code(self):
        status, result = self.request(method='POST', body='ssid=foo%0A%22%24%28touch+x%29&password=abc%22%5Cdefgh')
        self.assertEqual(status, 200)
        self.assertTrue(result['pending']); self.assertFalse(result['confirmed'])
        config = (self.runtime / 'pending.conf').read_text()
        self.assertIn('ssid=666f6f0a222428746f756368207829', config)
        self.assertIn('psk="abc\\"\\\\defgh"', config)
        self.assertEqual((self.runtime / 'pending.conf').stat().st_mode & 0o777, 0o600)
        self.assertNotIn('password', result)
        self.assertFalse((self.config / 'current.conf').exists())

    def test_confirm_requires_candidate_connected_and_unexpired(self):
        _, result = self.request(method='POST', body='ssid=Test&password=camera123')
        token = result['transaction']; body = 'transaction=' + token
        self.assertEqual(self.request('/api/wifi/confirm', 'POST', body)[0], 409)
        self.completed = True
        (self.runtime / 'active.id').write_text(token)
        self.assertEqual(self.request('/api/wifi/confirm', 'POST', body)[0], 200)
        self.assertTrue((self.config / 'current.conf').exists())
        self.assertFalse((self.runtime / 'pending.id').exists())
        self.assertEqual(self.request()[1]['ssid'], 'Test')

    def test_expired_or_wrong_transaction_does_not_replace_saved_wifi(self):
        _, result = self.request(method='POST', body='ssid=Test&password=camera123')
        (self.runtime / 'active.id').write_text(result['transaction']); self.completed = True
        (self.runtime / 'pending.expires').write_text('1')
        self.assertEqual(self.request('/api/wifi/confirm','POST','transaction='+result['transaction'])[0],409)
        self.assertFalse((self.config / 'current.conf').exists())
        self.assertEqual(self.request('/api/wifi/cancel','POST','transaction='+'0'*32)[0],409)

    def test_invalid_and_duplicate_fields_are_rejected(self):
        for body in ('ssid=Test&password=short','ssid=Test&ssid=Other&password=camera123','ssid=Test&password=abc%0Adefgh'):
            self.assertEqual(self.request(method='POST',body=body)[0],400)
        self.assertFalse((self.runtime / 'pending.conf').exists())

    def test_scan_uses_control_protocol_and_escapes_json(self):
        status, result = self.request('/api/wifi/scan','POST')
        self.assertEqual(status,200); self.assertTrue(result['scanning'])
        self.assertEqual(result['networks'][0]['ssid'],'Test "network"')
        self.assertEqual(result['networks'][0]['signal_dbm'],-42)
        self.assertIn('SCAN', self.commands)

    def test_recovery_command_is_queued_without_executing_shell(self):
        status,result=self.request('/api/wifi/recovery','POST','enabled=true')
        self.assertEqual(status,200)
        self.assertEqual((self.runtime/'request').read_text(),'recovery')
        self.assertFalse(result['active'])

    def test_recovery_password_is_only_returned_by_access_endpoint(self):
        self.assertNotIn('password',self.request('/api/wifi/recovery')[1])
        self.assertEqual(self.request('/api/wifi/recovery/access')[1]['password'],'camera123')


if __name__ == '__main__':
    unittest.main()
