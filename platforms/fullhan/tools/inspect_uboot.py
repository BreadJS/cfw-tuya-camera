#!/usr/bin/env python3
"""Reboot from a known Linux root shell and inspect U-Boot; never save/write flash."""
import argparse,os,re,signal,time
from pathlib import Path
import serial
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--allow-interruptible-boot',action='store_true',help='Use only after bootdelay was separately changed and authorized');p.add_argument('--pause-pid',type=int,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
if not args.allow_interruptible_boot:raise SystemExit('Factory bootdelay0 skips keyboard polling. No reboot attempted; review RAM-BOOT.md.')
if Path(f'/proc/{args.pause_pid}/comm').read_text().strip()!='picocom':raise SystemExit('Expected picocom process')
os.kill(args.pause_pid,signal.SIGSTOP)
try:
 with args.output.open('xb') as log:
  port=serial.Serial();port.port='/dev/ttyUSB0';port.baudrate=115200;port.timeout=.05;port.dtr=False;port.rts=False;port.open()
  try:
   def collect(seconds):
    end=time.monotonic()+seconds;data=b''
    while time.monotonic()<end:
     b=port.read(min(max(port.in_waiting,1),8192))
     if b:log.write(b);log.flush();data+=b
    return data
   collect(.2);port.write(b'\rsync; reboot -f\r');port.flush()
   deadline=time.monotonic()+35;window=b'';interrupted=False;last_key=0
   while time.monotonic()<deadline:
    b=port.read(min(max(port.in_waiting,1),8192))
    if b:
     log.write(b);log.flush();window=(window+b)[-32768:]
    if b'Starting kernel' in window:raise RuntimeError('Missed U-Boot window; no inspection commands sent')
    # This camera's default environment has no useful countdown interval.
    # Start queuing only space bytes after its U-Boot banner, before countdown.
    if b'U-Boot 2010.06' in window and time.monotonic()-last_key>.03:
     port.write(b' ');port.flush();last_key=time.monotonic()
    if b'U-Boot 2010.06' in window and re.search(rb'(?:^|[\r\n])U-Boot> *$',window):
     interrupted=True;break
   if not interrupted:raise RuntimeError('U-Boot prompt not observed')
   collect(.2);port.write(b'\r');collect(.3)
   confirmation=collect(.2);port.write(b'\r');confirmation+=collect(.3)
   if b'Starting kernel' in confirmation or not re.search(rb'(?:^|[\r\n])U-Boot> *$',confirmation):raise RuntimeError('Exact U-Boot prompt confirmation failed; no queries sent')
   for command in ('version','help','bdinfo','printenv bootargs bootcmd bootdelay loadaddr'):
    port.write(command.encode()+b'\r');port.flush();collect(1)
   print('U-Boot stopped and read-only inspection captured. No saveenv or flash commands sent.')
  finally:port.close()
finally:os.kill(args.pause_pid,signal.SIGCONT)
