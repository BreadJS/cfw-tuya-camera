#!/usr/bin/env python3
"""Decode valid H264 after constrained production-queue draining and recovery."""
import subprocess,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='rtsp-backpressure-') as td:
 d=Path(td);exe=d/'test';video=d/'input.h264';wire=d/'wire.rtp';decoded=d/'output.h264'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
  '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(HERE/'test_backpressure.c'),str(HERE.parent/'h264_parameters.c'),'-o',str(exe)],check=True)
 subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=1280x720:rate=15','-t','2',
  '-pix_fmt','yuv420p','-c:v','libx264','-preset','ultrafast','-tune','zerolatency','-qp','18',
  '-x264-params','aud=1:repeat-headers=1:keyint=15:bframes=0','-f','h264',str(video)],check=True,timeout=30)
 subprocess.run([str(exe),str(video),str(wire)],check=True,timeout=20)
 data=wire.read_bytes();at=0;sequence=None;fragment=False;markers=0;out=bytearray()
 while at<len(data):
  assert data[at]==36 and len(data)-at>=4
  channel=data[at+1];n=int.from_bytes(data[at+2:at+4],'big');at+=4
  packet=data[at:at+n];assert len(packet)==n;at+=n
  if channel!=0:continue
  assert packet[0]==0x80
  seq=int.from_bytes(packet[2:4],'big');assert sequence is None or seq==(sequence+1)%65536
  sequence=seq;payload=packet[12:];kind=payload[0]&31
  if kind==28:
   start=payload[1]&128;end=payload[1]&64
   if start:
    assert not fragment;fragment=True;out+=b'\x00\x00\x00\x01'+bytes([(payload[0]&0xe0)|(payload[1]&31)])
   else:assert fragment
   out+=payload[2:]
   if end:fragment=False
  else:
   assert not fragment and 1<=kind<=23;out+=b'\x00\x00\x00\x01'+payload
  if packet[1]&128:assert not fragment;markers+=1
 assert not fragment and markers>10
 decoded.write_bytes(out)
 result=subprocess.run(['ffmpeg','-v','error','-i',str(decoded),'-f','null','-'],capture_output=True,timeout=30)
 assert result.returncode==0 and not result.stderr,result.stderr.decode(errors='replace')
 print(f'Constrained 90KB/s transport: {markers} complete H264 frames; continuous RTP sequence; IDR recovery; FFmpeg decode without errors')
