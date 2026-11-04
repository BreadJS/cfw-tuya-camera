#!/usr/bin/env python3
import argparse,os,shlex,subprocess,tempfile
from pathlib import Path
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--sanitize',action='store_true');parser.add_argument('--arm-output',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parent
    compiler=shlex.split(os.environ.get('ARM_CC' if args.arm_output else 'CC','cc'))
    flags=['-std=c11','-O1' if args.sanitize else '-Os','-Wall','-Wextra','-Werror']
    if args.sanitize:flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    if args.arm_output:flags+=['-static','-s','-mcpu=arm1176jzf-s','-mfloat-abi=soft']
    with tempfile.TemporaryDirectory(prefix='camera-awb-') as directory:
        binary=args.arm_output or Path(directory)/'test'
        binary.parent.mkdir(parents=True,exist_ok=True)
        subprocess.run([*compiler,*flags,str(root/'isp_awb.c'),str(root/'test_isp_awb.c'),'-o',str(binary)],check=True)
        if not args.arm_output:subprocess.run([str(binary)],check=True,timeout=10)
if __name__=='__main__':main()
