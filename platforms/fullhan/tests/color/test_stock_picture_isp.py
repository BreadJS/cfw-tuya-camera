#!/usr/bin/env python3
"""Original ARM112a44 picture oracle, synthetic RAM only; optional Unicorn."""
import ctypes as C,hashlib,random,struct,subprocess,tempfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_R0,UC_ARM_REG_R1
ROOT=Path(__file__).resolve().parents[2]
image=(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes()
profile=(ROOT.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
class Config(C.Structure):_fields_=[('saturation',C.c_uint),('contrast',C.c_uint),('luma_offset',C.c_int),('hue_degrees',C.c_int),('cb_gain',C.c_uint),('cr_gain',C.c_uint),('temporal_nr',C.c_uint),('detail_percent',C.c_uint)]
class Write(C.Structure):_fields_=[('offset',C.c_uint32),('keep_mask',C.c_uint32),('value',C.c_uint32)]
class Plan(C.Structure):_fields_=[('writes',Write*10),('count',C.c_size_t)]
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0x10000,0x800000)
phoff=struct.unpack_from('<I',image,28)[0];phsize,phnum=struct.unpack_from('<HH',image,42)
for i in range(phnum):
 typ,off,addr,_,sz,_,_,_=struct.unpack_from('<8I',image,phoff+i*phsize)
 if typ==1:u.mem_write(addr,image[off:off+sz])
ctx,mm,stack,stop=0x70001000,0x72000000,0x71000000,0x60000000
for a,n in [(0x70000000,0x10000),(mm,0x4000),(stack,0x10000),(stop,0x1000)]:u.mem_map(a,n)
u.mem_write(ctx,profile);u.mem_write(ctx+0xa58,struct.pack('<I',mm));u.mem_write(0x59b36c,struct.pack('<I',ctx));u.mem_write(0x59b374,struct.pack('<I',ctx))
def hook(uc,pc,size,unused):
 if pc in (0x19f4c,0x19f7c):
  a,b=uc.reg_read(UC_ARM_REG_R0),uc.reg_read(UC_ARM_REG_R1)
  if pc==0x19f7c:
   a=a if a<0x80000000 else a-0x100000000;b=b if b<0x80000000 else b-0x100000000
  assert b;q=abs(a)//abs(b);q=-q if (a<0)!=(b<0) else q
  uc.reg_write(UC_ARM_REG_R0,q&0xffffffff);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc):
 u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop);u.emu_start(pc,stop,count=100000);assert u.reg_read(UC_ARM_REG_PC)==stop
with tempfile.TemporaryDirectory(prefix='stock-picture-') as td:
 lib=Path(td)/'picture.so'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(ROOT/'src/native/isp/picture_isp.c'),str(Path(__file__).with_name('oracle_driver.c')),'-o',str(lib)],check=True)
 c=C.CDLL(str(lib));c.fh_picture_isp_make_plan.argtypes=[C.POINTER(Config),C.POINTER(Plan)]
 random.seed(11244)
 vectors=[Config(320,60,0,hue,54,50,0,100) for hue in range(-64,64)]
 vectors+=[Config(random.randrange(256)*4,random.randrange(256),random.randrange(-128,128),random.randrange(-64,64),random.randrange(201),random.randrange(201),0,100) for _ in range(500)]
 for cfg in vectors:
  old=bytes(random.randrange(256) for _ in range(0x4000));u.mem_write(mm,old)
  u.mem_write(ctx+0x2ac,b'\0');u.mem_write(ctx+0x2b0,bytes([cfg.contrast,cfg.luma_offset&255]))
  u.mem_write(ctx+0x2d8,bytes([(cfg.hue_degrees*2)&255,cfg.saturation//4,cfg.cb_gain,cfg.cr_gain]))
  plan=Plan();assert not c.fh_picture_isp_make_plan(C.byref(cfg),C.byref(plan));native=bytearray(old)
  for w in list(plan.writes)[:7]:
   value=struct.unpack_from('<I',native,w.offset)[0];struct.pack_into('<I',native,w.offset,(value&w.keep_mask)|w.value)
  call(0x112a44);actual=bytes(u.mem_read(mm,0x4000))
  assert actual==native,[(hex(i),actual[i:i+4].hex(),native[i:i+4].hex()) for i in range(0,0x4000,4) if actual[i:i+4]!=native[i:i+4]]
 # Native percent detail policy matches original coefficient scaling at exact
 # quarter multipliers. Other detail tables/gates are deliberately untouched.
 for detail in range(0,201,25):
  cfg=Config(320,60,0,0,54,50,0,detail);plan=Plan();assert not c.fh_picture_isp_make_plan(C.byref(cfg),C.byref(plan))
  u.mem_write(ctx+0x60,struct.pack('<I',64<<12));u.mem_write(ctx+0x870,bytes([profile[0x870]&~4]));u.mem_write(ctx+0x874,struct.pack('<3H',20,16,16));u.mem_write(ctx+0x87a,bytes([detail*64//100]))
  call(0x112d08)
  assert struct.unpack('<I',u.mem_read(mm+0x4f8,4))[0]&0xffff0000==plan.writes[8].value
  assert struct.unpack('<I',u.mem_read(mm+0x4fc,4))[0]==plan.writes[9].value
 print(f'Original ARM112a44: {len(vectors)} complete MMIO vectors PASS;112d08:9 coefficient scaling vectors PASS')
