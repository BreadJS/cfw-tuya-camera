#!/usr/bin/env python3
import argparse,os,subprocess,tempfile
from pathlib import Path
def main():
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');p.add_argument('--arm-output',type=Path);a=p.parse_args()
    flags=['-std=c11','-O1'if a.sanitize else'-Os','-Wall','-Wextra','-Werror']
    if a.sanitize:flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    if a.arm_output:flags+=['-static','-s','-mcpu=arm1176jzf-s','-mfloat-abi=soft']
    cc=os.environ.get('ARM_CC'if a.arm_output else'CC','cc')
    with tempfile.TemporaryDirectory(prefix='rtsp-transport-test-')as t:
        out=a.arm_output or Path(t)/'test';out.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run([cc,*flags,str(Path(__file__).with_suffix('.c')),str(Path(__file__).resolve().parents[1]/'h264_parameters.c'),'-o',str(out)],check=True)
        if not a.arm_output:subprocess.run([str(out)],check=True,timeout=15)
if __name__=='__main__':main()
