#!/usr/bin/env python3
"""Execute the matching original ARM selector/CCM against synthetic RAM.
Requires optional Unicorn. No camera device or original program process runs.
"""
import ctypes as C,hashlib,os,random,struct,subprocess,tempfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_LR,UC_ARM_REG_SP,UC_ARM_REG_PC
ROOT=Path(__file__).resolve().parents[2]
image=(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes()
profile=(ROOT.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
class Gains(C.Structure):_fields_=[('red',C.c_uint32),('green',C.c_uint32),('blue',C.c_uint32)]
class Plan(C.Structure):_fields_=[('gain01',C.c_uint32),('gain23',C.c_uint32),('clip',C.c_uint32),('ccm',C.c_uint32*6),('first',C.c_uint),('second',C.c_uint),('weight',C.c_uint)]
def signed(v):return v if v<0x80000000 else v-0x100000000
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0x10000,0x800000)
phoff=struct.unpack_from('<I',image,28)[0];phsize,phnum=struct.unpack_from('<HH',image,42)
for i in range(phnum):
 typ,off,addr,_,sz,memsz,_,_=struct.unpack_from('<8I',image,phoff+i*phsize)
 if typ==1:u.mem_write(addr,image[off:off+sz])
context,stack,stop=0x70001000,0x71000000,0x60000000
u.mem_map(0x70000000,0x10000);u.mem_map(stack,0x10000);u.mem_map(stop,0x1000);u.mem_map(0x72000000,0x1000)
u.mem_write(context,profile);u.mem_write(0x59ae84,struct.pack('<I',context));u.mem_write(0x59af80,struct.pack('<I',context))
def hook(uc,pc,size,unused):
 if pc==0x19f4c:
  a,b=uc.reg_read(UC_ARM_REG_R0),uc.reg_read(UC_ARM_REG_R1);assert b;uc.reg_write(UC_ARM_REG_R0,a//b);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
 if pc==0x19f7c:
  a,b=signed(uc.reg_read(UC_ARM_REG_R0)),signed(uc.reg_read(UC_ARM_REG_R1));assert b
  q=abs(a)//abs(b);q=-q if (a<0)!=(b<0) else q
  uc.reg_write(UC_ARM_REG_R0,q&0xffffffff);uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc):
 u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop);u.emu_start(pc,stop,count=20000);assert u.reg_read(UC_ARM_REG_PC)==stop
with tempfile.TemporaryDirectory(prefix='camera-stock-ccm-') as td:
 lib=Path(td)/'color.so';source=ROOT/'src/native/isp'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(source/'isp_color_auto.c'),str(Path(__file__).with_name('oracle_driver.c')),'-o',str(lib)],check=True)
 c=C.CDLL(str(lib));c.fh_color_make_plan.argtypes=[C.POINTER(Gains),C.c_uint,*([C.c_uint32]*4),C.POINTER(Plan)]
 random.seed(7);vectors=[(512,512),(588,1483),(256,2048),(2048,256),(1024,1024)]+[(random.randint(256,2048),random.randint(256,2048)) for _ in range(300)]
 for red,blue in vectors:
  g=Gains(red,512,blue);p=Plan();assert c.fh_color_make_plan(C.byref(g),1,0x300030,0x300030,0x2590132,0xf6e0074,C.byref(p))==0
  u.mem_write(context+0xa8,struct.pack('<HH',512*4096//blue,512*4096//red));call(0x10abd4);call(0x10f2ec)
  coeff=list(struct.unpack('<6I',u.mem_read(context+0x140,24)))
  assert list(p.ccm)==coeff,(red,blue,list(p.ccm),coeff)
  first=u.mem_read(context+0xb1,1)[0]>>4;second=u.mem_read(context+0xb2,1)[0]&15;weight=struct.unpack('<H',u.mem_read(context+0xb0,2))[0]&4095
  assert (p.first,p.second,p.weight)==(first,second,weight),(red,blue,(p.first,p.second,p.weight),(first,second,weight))
 # Execute original gain writer as well, with unchanged direct logical gains.
 for red,blue in vectors[:5]:
  norm=306*512//red+601+116*512//blue
  logical=[norm*red//1024,norm*512//1024,norm*blue//1024]
  for bayer in range(4):
   g=Gains(red,512,blue);p=Plan();assert c.fh_color_make_plan(C.byref(g),bayer,0x300030,0x300030,0x2590132,0xf6e0074,C.byref(p))==0
   u.mem_write(context+0xa58,struct.pack('<I',0x72000000));u.mem_write(0x72000024,struct.pack('<I',bayer<<1));u.mem_write(0x72000084,struct.pack('<II',0x300030,0x300030))
   u.mem_write(0x656694,struct.pack('<3H',*logical));u.mem_write(0x656680,struct.pack('<I',0))
   u.reg_write(UC_ARM_REG_R0,logical[0]|logical[1]<<16);u.reg_write(UC_ARM_REG_R1,logical[2]);call(0x10bc2c)
   assert struct.unpack('<II',u.mem_read(0x72000224,8))==(p.gain01,p.gain23),(red,blue,bayer)
 print('Stock ARM gain writer:20 asymmetric/neutral Bayer/BLC publication vectors PASS')
 print(f'Stock ARM parity: {len(vectors)} logical gain pairs, selectors/weight and six CCM words PASS')
