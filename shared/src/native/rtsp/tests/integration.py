#!/usr/bin/env python3
"""Loopback-only tests; all media is generated, never read from the camera."""
import base64
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name('fixture_server.c')

def run(args, timeout=20):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=timeout)

class Client:
    def __init__(self, port, receive_buffer=None):
        self.sock = socket.socket()
        self.sock.settimeout(5)
        if receive_buffer is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.sock.connect(('127.0.0.1', port))
        self.buf = b''
        self.seq = 0
        self.url = f'rtsp://127.0.0.1:{port}/main'
        self.auth = 'Basic ' + base64.b64encode(b'test:fixture-password').decode()
    def read(self, size):
        while len(self.buf) < size:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError('server disconnected')
            self.buf += data
        data, self.buf = self.buf[:size], self.buf[size:]
        return data
    def request(self, method, suffix='', headers='', auth=True):
        self.seq += 1
        authorization = f'Authorization: {self.auth}\r\n' if auth else ''
        self.sock.sendall((f'{method} {self.url}{suffix} RTSP/1.0\r\nCSeq: {self.seq}\r\n'
                           + authorization + headers + '\r\n').encode())
        while b'\r\n\r\n' not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError(f'closed during {method}')
            self.buf += data
        head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        fields = {}
        lines = head.decode().split('\r\n')
        for line in lines[1:]:
            key, value = line.split(':', 1)
            fields[key.lower()] = value.strip()
        body = self.read(int(fields.get('content-length', '0')))
        return int(lines[0].split()[1]), fields, body
    def play(self):
        status, fields, _ = self.request('SETUP', '/trackID=0',
            'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n')
        assert status == 200, status
        session = fields['session'].split(';')[0]
        assert self.request('PLAY', headers=f'Session: {session}\r\n')[0] == 200
    def packet(self):
        header = self.read(4)
        assert header[0] == 36, header
        return header[1], self.read(struct.unpack('!H', header[2:])[0])
    def close(self):
        self.sock.close()

def protocol(port):
    c = Client(port)
    try:
        assert c.request('OPTIONS', auth=False)[0] == 401
        authorization = c.auth
        c.auth = 'Basic invalid'
        assert c.request('OPTIONS')[0] == 401
        c.auth = authorization
        assert c.request('DESCRIBE')[0] == 200
        assert c.request('SETUP', '/trackID=0', 'Transport: RTP/AVP;unicast;client_port=5000-5001\r\n')[0] == 461
        assert c.request('PLAY')[0] == 454
        c.play()
        previous_seq = None
        previous_stamp = None
        stamp = None
        completed = 0
        fragmented = False
        sender_report = False
        nal_start = False
        deadline = time.monotonic() + 5
        while completed < 4 and time.monotonic() < deadline:
            channel, packet = c.packet()
            if channel == 1:
                assert len(packet) == 72 and packet[1] == 200
                assert packet[28] == 0x81 and packet[29] == 202
                assert packet[36:38] == bytes([1,32])
                sender_report = True
            if channel != 0:
                continue
            assert len(packet) >= 13 and packet[0] == 0x80
            seq, timestamp = struct.unpack('!HI', packet[2:8])
            if previous_seq is not None:
                assert seq == (previous_seq + 1) % 65536
            previous_seq = seq
            if stamp is None:
                stamp = timestamp
                if previous_stamp is not None:
                    assert 5999 <= ((timestamp - previous_stamp) % 2**32) <= 6001
            assert timestamp == stamp, 'one source timestamp per access unit'
            payload = packet[12:]
            if payload[0] & 31 == 28:
                fragmented = True
                assert len(payload) > 2
                if payload[1] & 0x80:
                    assert not nal_start
                    nal_start = True
                else:
                    assert nal_start
                if payload[1] & 0x40:
                    nal_start = False
            if packet[1] & 0x80:
                assert not nal_start
                completed += 1
                previous_stamp, stamp = stamp, None
        assert completed == 4 and fragmented and sender_report
    finally:
        c.close()

def main():
    with tempfile.TemporaryDirectory(prefix='camera-rtsp-test-') as directory:
        d = Path(directory)
        fixture_binary = os.environ.get('SERVER_BIN')
        if not fixture_binary:
            fixture_binary = str(d/'fixture_server')
            run([os.environ.get('CC','cc'), '-std=c11', '-O2', '-I'+str(ROOT),
                 str(ROOT/'camera_rtsp.c'), str(ROOT/'h264_parameters.c'), str(FIXTURE), '-o', fixture_binary])
        for name, dimensions in [('main', '1920x1080'), ('sub', '640x360')]:
            run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'testsrc2=size={dimensions}:rate=15',
                 '-t', '2', '-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-preset', 'ultrafast',
                 '-tune', 'zerolatency', '-x264-params', 'aud=1:repeat-headers=1:keyint=15:bframes=0',
                 '-f', 'h264', str(d/f'{name}.h264')])
        for codec, rate, encoder, fmt in [('pcma', 8000, 'pcm_alaw', 'alaw'),
                                           ('l16', 16000, 'pcm_s16be', 's16be')]:
            run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'sine=frequency=440:sample_rate={rate}',
                 '-t', '2', '-c:a', encoder, '-f', fmt, str(d/f'{codec}.raw')])
        credentials = d/'credentials'
        credentials.write_text('test:fixture-password\n')
        credentials.chmod(0o600)
        for codec, rate, expected, name, dimensions in [('pcma', 8000, 'pcm_alaw', 'main', (1920,1080)),
                                                     ('l16', 16000, 'pcm_s16be', 'sub', (640,360))]:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            command = [fixture_binary, '--video', str(d/f'{name}.h264'),
                       '--audio', str(d/f'{codec}.raw'), '--audio-codec', codec,
                       '--audio-rate', str(rate), '--port', str(port), '--auth-file', str(credentials)]
            server = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                for _ in range(100):
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.1):
                            break
                    except OSError:
                        if server.poll() is not None:
                            raise RuntimeError(server.stderr.read().decode())
                        time.sleep(.02)
                protocol(port)
                slow = Client(port, receive_buffer=1024)
                slow.play()
                url = f'rtsp://test:fixture-password@127.0.0.1:{port}/main'
                probe = json.loads(run(['ffprobe', '-v', 'error', '-rtsp_transport', 'tcp',
                    '-analyzeduration', '1000000', '-show_streams', '-of', 'json', url]).stdout)
                streams = probe['streams']
                assert streams[0]['codec_name'] == 'h264'
                assert (streams[0]['width'],streams[0]['height']) == dimensions
                assert streams[1]['codec_name'] == expected and int(streams[1]['sample_rate']) == rate
                # Two separate decodes prove a client may disconnect and reconnect at a fresh IDR.
                for _ in range(2):
                    decoded = run(['ffmpeg', '-v', 'info', '-rtsp_transport', 'tcp', '-i', url,
                        '-t', '1.2', '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'], timeout=15)
                    assert b'frame=' in decoded.stderr and b'video:0kB' not in decoded.stderr
                # A reader that stopped consuming must eventually be disconnected;
                # the FFmpeg clients above still decoded while it was stalled.
                slow.sock.settimeout(3)
                while slow.sock.recv(65536):
                    pass
                slow.close()
                # Oversized request cannot affect subsequent connections.
                broken = socket.create_connection(('127.0.0.1', port), timeout=3)
                broken.sendall(b'OPTIONS '+b'x'*9000)
                time.sleep(.1)
                broken.close()
                protocol(port)
                print(f'PASS {name} {codec}: auth, RTSP state, FU-A, AU timestamps, probe, A/V decode, reconnect, slow reader isolation, bounded headers', flush=True)
            finally:
                server.send_signal(signal.SIGTERM)
                try:
                    server.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.communicate()
                assert server.returncode == 0, server.returncode

if __name__ == '__main__':
    main()
