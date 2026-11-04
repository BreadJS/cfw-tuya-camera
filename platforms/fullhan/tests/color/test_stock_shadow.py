#!/usr/bin/env python3
"""Exact original1112ac state/register oracle in synthetic RAM; no camera.
uv run --with unicorn python tests/color/test_stock_shadow.py
"""
import ctypes as C,hashlib,random,re,struct,subprocess,tempfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[2]
image=(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes();profile=(ROOT.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
class Config(C.Structure):_fields_=[(n,C.c_uint8) for n in ['ae_target','shadow_bias','low_scale','middle_scale','high_scale']]
class State(C.Structure):_fields_=[('generation',C.c_uint64)]+[(n,C.c_uint32) for n in ['previous_min','previous_max','smooth_min','smooth_max','hist0','hist2','mean_mass']]
class Input(C.Structure):_fields_=[('generation',C.c_uint64),('width',C.c_uint32),('height',C.c_uint32),('histogram',C.c_uint32*3)]+[(n,C.c_uint32) for n in ['min_log_q8','max_log_q8','reg23c','reg240','reg244','reg248','reg24c','reg250']]
class Plan(C.Structure):_fields_=[(n,C.c_uint32) for n in ['reg23c','reg240','reg244','reg248','reg24c','reg250']]
class Tile(C.Structure):_fields_=[('sum',C.c_uint32*4),('count',C.c_uint32)]
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
 if pc==0x1111a4 and not extrema_only:
  u.mem_write(0x59b008,struct.pack('<II',current.min_log_q8,current.max_log_q8));ret(());return
 name=helpers.get(pc)
 if name is None:return
 a,b,c,d=[uc.reg_read(r) for r in regs]
 if name=='__aeabi_uidiv':assert b;ret(a//b)
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
with tempfile.TemporaryDirectory(prefix='camera-stock-shadow-') as td:
 lib=Path(td)/'shadow.so';source=ROOT/'src/native/isp'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-ffp-contract=off','-shared','-fPIC',str(source/'isp_shadow.c'),'-o',str(lib)],check=True)
 c=C.CDLL(str(lib));c.fh_shadow_make_plan.argtypes=[C.POINTER(Config),C.POINTER(State),C.POINTER(Input),C.POINTER(Plan)]
 c.fh_shadow_extrema.argtypes=[C.POINTER(Tile),C.c_uint,C.POINTER(C.c_uint32),C.POINTER(C.c_uint32)]
 random.seed(1112);count=0
 offsets=[0x23c,0x240,0x244,0x248,0x24c,0x250]
 for cfg in [Config(106,10,64,64,255),Config(60,0,50,70,128),Config(0,3,0,0,0)]:
  state=State();u.mem_write(0x59afd8,b'\0'*(0x59b038-0x59afd8))
  for off,val in zip([0x66,0x265,0x268,0x269,0x26a],[cfg.ae_target,cfg.shadow_bias,cfg.low_scale,cfg.middle_scale,cfg.high_scale]):u.mem_write(ctx+off,bytes([val]))
  for generation in range(1,251):
   width,height=random.choice([(1920,1080),(640,360),(1280,720)])
   current=Input(generation,width,height,(C.c_uint32*3)(random.randrange(65536)*(width*height>>6),random.randrange(4096)*(width*height>>2),random.randrange(65536)),random.randrange(4000),random.randrange(4000,4096),*[random.getrandbits(32) for _ in offsets]);plan=Plan()
   for off,name in zip(offsets,[x[0] for x in Plan._fields_]):u.mem_write(mm+off,struct.pack('<I',getattr(current,name)))
   u.mem_write(mm+0x30,struct.pack('<I',width|(height<<16)));u.mem_write(hist,struct.pack('<3I',*current.histogram))
   assert c.fh_shadow_make_plan(C.byref(cfg),C.byref(state),C.byref(current),C.byref(plan))==0
   call(0x1112ac)
   actual=[struct.unpack('<I',u.mem_read(mm+off,4))[0] for off in offsets];expected=[getattr(plan,n) for n,_ in Plan._fields_]
   assert actual==expected,(generation,bytes(cfg),actual,expected)
   # Exact state observable by subsequent calls, not just one-frame output.
   actualstate=[struct.unpack('<I',u.mem_read(addr,4))[0] for addr in [0x59aff0,0x59aff4,0x59b020,0x59b024,0x59b028,0x59b02c,0x59b030]]
   assert actualstate==[getattr(state,n) for n,_ in State._fields_[1:]],(generation,actualstate,list(bytes(state)))
   count+=1
 extrema_only=True
 for iteration in range(50):
  rows,cols=15,15;n=(rows+1)*(cols+1)-1
  values=(Tile*n)()
  for i in range(n):
   values[i]=Tile((C.c_uint32*4)(*[random.randrange(200000) for _ in range(4)]),random.randrange(100,1000))
   for plane in range(4):u.mem_write(tiles+plane*0x1000+i*16,struct.pack('<II',values[i].sum[plane],values[i].count))
  lo,hi=C.c_uint32(),C.c_uint32();assert c.fh_shadow_extrema(values,n,C.byref(lo),C.byref(hi))==0
  u.mem_write(mm+0x1cc,struct.pack('<I',(rows<<24)|(cols<<16)));call(0x1111a4)
  assert struct.unpack('<II',u.mem_read(0x59b008,8))==(lo.value,hi.value)
 print(f'Original ARM1112ac:{count} consecutive stateful plans and six register words PASS;1111a4:50x255tile extrema PASS')
