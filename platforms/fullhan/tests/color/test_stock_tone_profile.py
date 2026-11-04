#!/usr/bin/env python3
"""Exact original111a88 static-tone oracle in synthetic RAM; no camera.
uv run --with unicorn python tests/color/test_stock_tone_profile.py
"""
import ctypes as C,hashlib,random,re,struct,subprocess,tempfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[2]
image=(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes();profile=(ROOT.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0x10000,0x800000)
phoff=struct.unpack_from('<I',image,28)[0];phsize,phnum=struct.unpack_from('<HH',image,42)
for i in range(phnum):
 typ,off,addr,_,sz,memsz,_,_=struct.unpack_from('<8I',image,phoff+i*phsize)
 if typ==1:u.mem_write(addr,image[off:off+sz])
ctx,mm,stack,stop,hist,tiles=0x70001000,0x72000000,0x71000000,0x60000000,0x73000000,0x74000000
for a,sz in [(0x70000000,0x10000),(mm,0x4000),(stack,0x10000),(stop,0x1000),(hist,0x1000),(tiles,0x10000)]:u.mem_map(a,sz)
u.mem_write(ctx,profile);u.mem_write(ctx+0xa58,struct.pack('<I',mm));u.mem_write(0x59afd4,struct.pack('<I',ctx))
rels=subprocess.check_output(['readelf','-r',str(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin')],text=True)
names=re.findall(r'R_ARM_JUMP_SLOT\s+\S+\s+(\S+)',rels)
helpers={0x18cfc+20+12*i:name.split('@')[0] for i,name in enumerate(names)}
assert helpers[0x18ddc]=='__aeabi_uldivmod';assert helpers[0x19f4c]=='__aeabi_uidiv'
current=None;extrema_only=False
regs=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]
def signed(v):return v if v<0x80000000 else v-0x100000000
def double(lo,hi):return struct.unpack('<d',struct.pack('<II',lo,hi))[0]
def hook(uc,pc,size,unused):
 def ret(v):
  if isinstance(v,float):v=struct.unpack('<II',struct.pack('<d',v))
  if not isinstance(v,tuple):v=(v,)
  for reg,value in zip(regs,v):uc.reg_write(reg,value&0xffffffff)
  uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
 if pc==0x107c48:ret(hist);return
 if pc==0x107ae0:ret(tiles);return
 if pc in (0x1111a4,0x1112ac):ret(());return
 name=helpers.get(pc)
 if name is None:return
 a,b,c,d=[uc.reg_read(r) for r in regs]
 if name=='__aeabi_uidiv':assert b;ret(a//b)
 elif name=='__aeabi_idiv':
  a,b=signed(a),signed(b);q=abs(a)//abs(b);ret(-q if (a<0)!=(b<0) else q)
 elif name=='__aeabi_uldivmod':
  q,r=divmod(a|(b<<32),c|(d<<32));ret((q&0xffffffff,q>>32,r&0xffffffff,r>>32))
 elif name=='__aeabi_ui2d':ret(float(a))
 elif name=='__aeabi_i2d':ret(float(signed(a)))
 elif name=='__aeabi_dmul':ret(double(a,b)*double(c,d))
 elif name=='__aeabi_dadd':ret(double(a,b)+double(c,d))
 elif name=='__aeabi_dsub':ret(double(a,b)-double(c,d))
 elif name=='__aeabi_ddiv':ret(double(a,b)/double(c,d))
 elif name=='__aeabi_dcmpgt':ret(int(double(a,b)>double(c,d)))
 elif name=='__aeabi_d2uiz':ret(int(double(a,b)))
 else:raise AssertionError((hex(pc),name))
u.hook_add(UC_HOOK_CODE,hook)
def call(pc):
 u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop);u.emu_start(pc,stop,count=100000);assert u.reg_read(UC_ARM_REG_PC)==stop
class Write(C.Structure):_fields_=[('offset',C.c_uint32),('keep_mask',C.c_uint32),('value',C.c_uint32)]
class Plan(C.Structure):_fields_=[('writes',Write*44),('count',C.c_size_t),('curve_index',C.c_uint),('secondary_index',C.c_uint)]
with tempfile.TemporaryDirectory(prefix='camera-stock-tone-') as td:
 lib=Path(td)/'tone.so';source=ROOT/'src/native/isp'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(source/'isp_tone_profile.c'),'-o',str(lib)],check=True)
 c=C.CDLL(str(lib));c.fh_tone_profile_make_plan.argtypes=[C.c_uint,C.POINTER(Plan)]
 random.seed(11188);indices=set()
 for gain in range(64,1025):
  old=bytes(random.randrange(256) for _ in range(0x4000));u.mem_write(mm,old);u.mem_write(ctx+0x60,struct.pack('<I',gain<<12))
  native=bytearray(old);plan=Plan();assert c.fh_tone_profile_make_plan(gain,C.byref(plan))==0 and plan.count==44
  for w in plan.writes:
   value=struct.unpack_from('<I',native,w.offset)[0];struct.pack_into('<I',native,w.offset,(value&w.keep_mask)|w.value)
  call(0x111a88)
  actual=bytes(u.mem_read(mm,0x4000))
  if actual!=native:
   changes=[(hex(i),hex(struct.unpack_from('<I',actual,i)[0]),hex(struct.unpack_from('<I',native,i)[0])) for i in range(0,0x4000,4) if actual[i:i+4]!=native[i:i+4]]
   raise AssertionError((gain,plan.curve_index,plan.secondary_index,changes))
  indices.add((plan.curve_index,plan.secondary_index))
 print(f'Original ARM111a88 static-tone:961 gains64..1024;44 writes;all MMIO including unrelated bits PASS;indices {sorted(indices)}')
