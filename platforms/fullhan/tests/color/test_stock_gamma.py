#!/usr/bin/env python3
"""Optional original ARM gamma oracle; synthetic RAM, no camera access."""
from pathlib import Path
import struct,hashlib
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
r=Path(__file__).resolve().parents[2];image=(r.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes();profile=(r.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0x10000,0x800000);off=struct.unpack_from('<I',image,28)[0];sz,n=struct.unpack_from('<HH',image,42)
for i in range(n):
 typ,fo,va,pa,fs,ms,flags,al=struct.unpack_from('<8I',image,off+i*sz)
 if typ==1:u.mem_write(va,image[fo:fo+fs])
ctx,mm,stack,stop=0x70001000,0x72000000,0x71000000,0x60000000
for addr,size in [(0x70000000,0x10000),(mm,0x4000),(stack,0x10000),(stop,0x1000)]:u.mem_map(addr,size)
u.mem_write(ctx,profile);u.mem_write(ctx+0xa58,struct.pack('<I',mm));u.mem_write(0x59afa0,struct.pack('<I',ctx));u.mem_write(mm+0x4f0,struct.pack('<I',0x31))
u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop);u.emu_start(0x110840,stop,count=10000);assert u.reg_read(UC_ARM_REG_PC)==stop
words=list(struct.unpack('<160I',u.mem_read(ctx+0x5f0,640)))
meta=struct.unpack('<2I',u.mem_read(0x59afac,8));assert meta==(1,1)
assert struct.unpack('<I',u.mem_read(mm+0x4f0,4))[0]==0x21
for bank in [words[:80],words[80:]]:
 values=[v for w in bank for v in [w&65535,w>>16]];assert max(values)<=4095 and all(a<=b for a,b in zip(values,values[1:]))
import re
header=(r/'src/native/isp/gamma/day_gamma.h').read_text().split('={',1)[1].split('};',1)[0]
actual=[int(x[:-1],0) for x in re.findall(r'(?:0x[0-9a-f]+|[0-9]+)u',header)]
assert actual==words+[1,1,1]
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
print('Original ARM gamma dispatcher parity:163words, monotonic banks, enable flags and mode route PASS')
presets_header=(r/'src/native/isp/gamma/presets.h').read_text().split('={',1)[1].split('};',1)[0]
presets=[int(x[:-1],0) for x in re.findall(r'(?:0x[0-9a-f]+|[0-9]+)u',presets_header)]
assert len(presets)==800
for index in range(10):
 u.mem_write(ctx,profile);u.mem_write(ctx+0xa58,struct.pack('<I',mm))
 u.mem_write(ctx+0x364,bytes([(profile[0x364]&15)|(index<<4)]))
 u.mem_write(mm+0x4f0,struct.pack('<I',0xabcdef31))
 u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop)
 u.emu_start(0x110840,stop,count=10000);assert u.reg_read(UC_ARM_REG_PC)==stop
 banks=list(struct.unpack('<160I',u.mem_read(ctx+0x5f0,640)))
 assert banks[:80]==actual[:80] and banks[80:]==presets[index*80:(index+1)*80],index
 assert struct.unpack('<2I',u.mem_read(0x59afac,8))==(1,1)
 assert struct.unpack('<I',u.mem_read(mm+0x4f0,4))[0]==0xabcdef21
 for bank in (banks[:80],banks[80:]):
  values=[v for word in bank for v in (word&65535,word>>16)]
  assert min(values)>=0 and max(values)<=4095 and all(a<=b for a,b in zip(values,values[1:])),index
print('Original ARM gamma dispatcher: all10 presets match complete160-word banks and preserved route bits PASS')
