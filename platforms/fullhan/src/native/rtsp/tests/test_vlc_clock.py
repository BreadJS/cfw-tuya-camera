#!/usr/bin/env python3
"""Optional VLC3 playback-clock reproduction, synthetic media and loopback only.

This changes only a private test server's CLOCK_REALTIME query; it never changes
the host clock, real camera, production source or monotonic media timestamps.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name('fixture_server.c')
SHIM = '''#include <time.h>
int fixture_clock_gettime(clockid_t id,struct timespec *value)
{
    int rc=clock_gettime(id,value);
    if(!rc && id==CLOCK_REALTIME)value->tv_sec-=10800;
    return rc;
}
'''

def run(arguments):
    subprocess.run(arguments, check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, timeout=30)

def compare(directory, vlc, fixed_seconds=None):
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    (directory/'clock.c').write_text(SHIM if fixed_seconds is None else SHIM.replace('value->tv_sec-=10800',f'value->tv_sec={fixed_seconds}'))
    run([*compiler, '-O2', '-std=c11', '-Dclock_gettime=fixture_clock_gettime',
         '-c', str(ROOT/'camera_rtsp.c'), '-o', str(directory/'rtspunsynced.o')])
    run([*compiler, '-O2', '-D_POSIX_C_SOURCE=200809L', str(directory/'clock.c'),
         str(FIXTURE), str(directory/'rtspunsynced.o'), '-o', str(directory/'serverunsynced')])
    run([*compiler, '-O2', str(ROOT/'camera_rtsp.c'), str(FIXTURE),
         '-o', str(directory/'servercurrent')])
    run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=15',
         '-t', '2', '-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-preset', 'ultrafast',
         '-tune', 'zerolatency', '-x264-params', 'aud=1:repeat-headers=1:keyint=15:bframes=0',
         '-f', 'h264', str(directory/'video.h264')])
    run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=8000',
         '-t', '2', '-c:a', 'pcm_alaw', '-f', 'alaw', str(directory/'audio.raw')])
    jobs = []
    try:
        for label in ['current', 'unsynced']:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            error = open(directory/f'server-{label}.log', 'wb')
            server = subprocess.Popen([str(directory/f'server{label}'), '--video', str(directory/'video.h264'),
                '--audio', str(directory/'audio.raw'), '--audio-codec', 'pcma', '--port', str(port)],
                stdout=subprocess.DEVNULL, stderr=error)
            job = {'label': label, 'server': server, 'files': [error]}
            jobs.append(job)
            for _ in range(100):
                try:
                    with socket.create_connection(('127.0.0.1', port), .1):
                        break
                except OSError:
                    assert server.poll() is None, 'test server exited'
                    time.sleep(.02)
            output = open(directory/f'vlc-{label}.out', 'wb')
            log = open(directory/f'vlc-{label}.log', 'wb')
            job['files'] += [output, log]
            job['vlc'] = subprocess.Popen([vlc, '--no-one-instance', '--ignore-config',
                '--intf', 'rc', '--rc-fake-tty', '--avcodec-hw=none', '--vout=dummy', '--aout=dummy',
                '--no-osd', '--no-video-title-show', '--rtsp-tcp', '--network-caching=300', '-vv',
                f'rtsp://127.0.0.1:{port}/main'], stdin=subprocess.PIPE, stdout=output, stderr=log)
        time.sleep(7)
        for job in jobs:
            job['vlc'].stdin.write(b'stats\n');job['vlc'].stdin.flush()
        time.sleep(5)
        for job in jobs:
            job['vlc'].stdin.write(b'stats\n');job['vlc'].stdin.flush()
        time.sleep(1)
        for job in jobs:
            process = job['vlc']
            if process.poll() is None:
                process.communicate(input=b'quit\n', timeout=3)
            for file in job['files']:
                file.flush()
        result = {}
        for job in jobs:
            label = job['label']
            data = (directory/f'vlc-{label}.log').read_bytes()
            text = data.decode(errors='replace')
            stats = (directory/f'vlc-{label}.out').read_text(errors='replace')
            result[label] = {'sha256': hashlib.sha256(data).hexdigest(),
                'displayed': [int(x) for x in re.findall(r'frames displayed\s*:\s*(\d+)',stats)],
                'audio_played': [int(x) for x in re.findall(r'buffers played\s*:\s*(\d+)',stats)],
                'buffering_completed': 'Stream buffering done' in text,
                'timestamp_conversion_failed': 'Timestamp conversion failed' in text or
                                               'Could not convert timestamp' in text,
                'decoder_errors': [line for line in text.splitlines() if 'decoder error:' in line]}
        (directory/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, indent=2))
        for label in ['current','unsynced']:
            assert result[label]['buffering_completed'], label
            assert not result[label]['timestamp_conversion_failed'], label
            for field in ['displayed','audio_played']:
                values=result[label][field]
                assert len(values)==2 and values[1]>values[0]>0, (label,field,values)
        print('PASS VLC3: video and audio advance with correct and unsynchronized camera clocks')
    finally:
        for job in jobs:
            for key in ['vlc', 'server']:
                process = job.get(key)
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            for file in job['files']:
                file.close()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vlc', default=shutil.which('vlc'))
    parser.add_argument('--fixed-seconds', type=int, help='replace the unsynchronized camera realtime with this Unix second')
    parser.add_argument('--output-dir', type=Path, help='new directory in which to retain generated test artifacts')
    args = parser.parse_args()
    if not args.vlc or not shutil.which('ffmpeg'):
        parser.error('optional test requires VLC3 and FFmpeg/libx264')
    version = subprocess.run([args.vlc, '--version'], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=10).stdout.decode(errors='replace')
    if 'VLC version 3.' not in version:
        parser.error('this reproduction specifically targets VLC3/live555')
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        compare(args.output_dir.resolve(), args.vlc, args.fixed_seconds)
    else:
        with tempfile.TemporaryDirectory(prefix='camera-vlc-clock-') as directory:
            compare(Path(directory), args.vlc, args.fixed_seconds)

if __name__ == '__main__':
    main()
