#!/usr/bin/env python3
"""Optional Unicorn oracle: execute exact shipped mode1 against synthetic RAM.
No camera access. Native dark/clipping/bounds/smoothing policy tested separately.
Run: uv run --with unicorn python tests/color/test_stock_awb_mode1.py
"""
import ctypes as C,hashlib,random,struct,subprocess,tempfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[2]
image=(ROOT.parent/'ipcam_hack_extracted/live_camera/dgiot.bin').read_bytes()
profile=(ROOT.parent/'ipcam_hack_extracted/rootfs/usr/bin/day_mis2006_mipi_attr.hex').read_bytes()
assert hashlib.sha256(image).hexdigest()=='bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714'
assert hashlib.sha256(profile).hexdigest()=='467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba'
class Gains(C.Structure):_fields_=[('red',C.c_uint32),('green',C.c_uint32),('blue',C.c_uint32)]
class Cell(C.Structure):_fields_=[('red',C.c_uint32),('green',C.c_uint32),('blue',C.c_uint32),('count',C.c_uint32)]
class Sample(C.Structure):_fields_=[('generation',C.c_uint64),('cell',Cell*9)]
class Config(C.Structure):_fields_=[('minimum_mean',C.c_uint32),('saturation_mean',C.c_uint32),('minimum_cells',C.c_uint),('spread_low',C.c_uint8),('spread_middle',C.c_uint8),('spread_high',C.c_uint8)]
class Result(C.Structure):_fields_=[('gains',Gains),('pre_luma',Gains),('accepted_cells',C.c_uint),('selected_cell',C.c_uint),('selected_weight',C.c_uint),('changed',C.c_uint)]
class State(C.Structure):_fields_=[('generation',C.c_uint64),('gains',Gains),('initialized',C.c_uint)]
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.mem_map(0x10000,0x800000)
phoff=struct.unpack_from('<I',image,28)[0];phsize,phnum=struct.unpack_from('<HH',image,42)
for i in range(phnum):
 typ,off,addr,_,sz,memsz,_,_=struct.unpack_from('<8I',image,phoff+i*phsize)
 if typ==1:u.mem_write(addr,image[off:off+sz])
context,stack,stop=0x70001000,0x71000000,0x60000000
u.mem_map(0x70000000,0x10000);u.mem_map(stack,0x10000);u.mem_map(stop,0x1000)
u.mem_write(context,profile);u.mem_write(0x59ae84,struct.pack('<I',context))
captured=[];ranked=[]
def hook(uc,pc,size,unused):
 if pc==0x18ddc:
  a=uc.reg_read(UC_ARM_REG_R0)|(uc.reg_read(UC_ARM_REG_R1)<<32);b=uc.reg_read(UC_ARM_REG_R2)|(uc.reg_read(UC_ARM_REG_R3)<<32)
  assert b;q,r=divmod(a,b)
  for reg,val in zip([UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3],[q&0xffffffff,q>>32,r&0xffffffff,r>>32]):uc.reg_write(reg,val)
  uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
 elif pc==0x10b094:uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR)) # external filter publication skipped
 elif pc==0x10ab8c:ranked[:]=[uc.reg_read(UC_ARM_REG_R3)]
 elif pc==0x10b6c4:
  captured[:]=[uc.reg_read(r) for r in (UC_ARM_REG_R4,UC_ARM_REG_R6,UC_ARM_REG_R5)];uc.emu_stop()
u.hook_add(UC_HOOK_CODE,hook)
with tempfile.TemporaryDirectory(prefix='camera-stock-mode1-') as td:
 lib=Path(td)/'mode1.so';source=ROOT/'src/native/isp'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(source/'isp_awb_mode1.c'),str(source/'isp_awb.c'),'-o',str(lib)],check=True)
 c=C.CDLL(str(lib));c.fh_awb_mode1_target.argtypes=[C.POINTER(Config),C.POINTER(Sample),C.POINTER(Result)]
 c.fh_awb_mode1_plan.argtypes=[C.POINTER(Config),C.POINTER(State),C.POINTER(Sample),C.POINTER(Result)]
 cfg=Config(16,4090,3,255,255,255);random.seed(16)
 vectors=[[(1000+6*i,1200,1000,100) for i in range(9)]]
 for ratio in [3298,3299,3300,3684,3685,3686,4096]:
  vectors.append([(1000*ratio//4096,1200,1000,100)]*9)
 for _ in range(700):
  cells=[]
  for i in range(9):
   n=random.choice([0,10,11,100,10000]);cells.append((random.randint(40,3000),random.randint(40,3000),random.randint(40,3000),n))
  if sum(x[3]>=11 for x in cells)>=3 and sum(x[3] for x in cells)>=90:vectors.append(cells)
 weights=set();checked=0
 for thresholds in [(255,255,255),(0,8,16),(1,2,4)]:
  cfg.spread_low,cfg.spread_middle,cfg.spread_high=thresholds
  u.mem_write(context+0x75,bytes(thresholds))
  for cells in vectors:
   s=Sample(1,(Cell*9)(*[Cell(r*n,g*n,b*n,n) for r,g,b,n in cells]));result=Result()
   assert c.fh_awb_mode1_target(C.byref(cfg),C.byref(s),C.byref(result))==1
   u.mem_write(0x59ae8c,b''.join(struct.pack('<4I',x.red,x.green,x.blue,x.count) for x in s.cell));u.mem_write(0x656670,b'\0'*4)
   captured.clear();ranked.clear();u.reg_write(UC_ARM_REG_SP,stack+0xf000);u.reg_write(UC_ARM_REG_LR,stop);u.reg_write(UC_ARM_REG_R0,context+0x900)
   u.emu_start(0x10b160,stop,count=100000)
   native=[result.pre_luma.red,result.pre_luma.green,result.pre_luma.blue]
   assert captured==native,(thresholds,cells,captured,native,result.selected_weight,result.selected_cell)
   assert ranked==[result.selected_cell],(ranked,result.selected_cell)
   weights.add(result.selected_weight);checked+=1
 cfg.spread_low=cfg.spread_middle=cfg.spread_high=255
 # All constant-ratio ties and invalid-middle ranking, including nonzero
 # spread thresholds, have identical target gains; checks above cover ties.
 dark=Sample(2,(Cell*9)(*[Cell(100,100,100,100)]*9));state=State(0,Gains(600,512,1300),1);result=Result()
 assert c.fh_awb_mode1_plan(C.byref(cfg),C.byref(state),C.byref(dark),C.byref(result))==0
 assert (state.gains.red,state.gains.blue,state.generation)==(600,1300,2)
 assert c.fh_awb_mode1_plan(C.byref(cfg),C.byref(state),C.byref(dark),C.byref(result))==-116
 lit=Sample(3,(Cell*9)(*[Cell(100000,100000,100000,100)]*9));assert c.fh_awb_mode1_plan(C.byref(cfg),C.byref(state),C.byref(lit),C.byref(result))==1
 assert 0<600-state.gains.red<=6 and 0<1300-state.gains.blue<=32
 clipped=Sample(4,(Cell*9)(*[Cell(409000,100000,100000,100)]*9));assert c.fh_awb_mode1_plan(C.byref(cfg),C.byref(state),C.byref(clipped),C.byref(result))==0
 print(f'Original ARM mode1 target: {checked} synthetic9-cell/config vectors PASS; weights {sorted(weights)}; native dark/clipping/freshness/smoothing PASS')
