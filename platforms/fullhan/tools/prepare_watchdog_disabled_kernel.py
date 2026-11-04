#!/usr/bin/env python3
"""Exact-image offline patch: stock watchdog init -> vendor pause, return0.

Produces a RAM-test candidate only. No camera, UART, MTD or flash access.
"""
import hashlib,json,lzma,struct,zlib
from pathlib import Path
import firmware_package
ROOT=Path(__file__).resolve().parents[1]
BASE_SHA='7a64faa44c9f0dc7b8f2ab21b67078bcb55f792684363c920ecddcf0819a9d37'
RAW_SHA='d92abc48d977e41b4c5c430916a4cdc89699e2448c9124ec379b44bf96fadf58'
VIRTUAL_BASE=0xc0008000
INIT=0xc04bf474
PAUSE=0xc001a800
XZ_OFFSET=15316
XZ_BYTES=1883900
EXPECTED=bytes.fromhex('0010a0e300009fe5a113f5eaa0a04ec0')

def main():
    original=(ROOT/'base/kernel.uImage').read_bytes()
    if hashlib.sha256(original).hexdigest()!=BASE_SHA:raise ValueError('Unknown input kernel; no patch')
    firmware_package.check_image(2,original)
    body=original[64:];decoder=lzma.LZMADecompressor();raw=decoder.decompress(body[XZ_OFFSET:])
    if not decoder.eof or len(decoder.unused_data)!=64 or len(body)-XZ_OFFSET-64!=XZ_BYTES:raise ValueError('Unexpected compressed layout')
    if hashlib.sha256(raw).hexdigest()!=RAW_SHA:raise ValueError('Unknown decompressed image')
    offset=INIT-VIRTUAL_BASE
    if raw[offset:offset+16]!=EXPECTED:raise ValueError('Watchdog init/literal mismatch')
    # AAPCS: retain an8-byte-aligned stack, preserve r4/lr, call the exact
    # stock board pause routine (PMU0x1fc bit8), and return successful init.
    displacement=PAUSE-(INIT+4+8)
    if displacement%4 or not -(1<<25)<=displacement<(1<<25):raise ValueError('Branch out of range')
    words=(0xe92d4010,0xeb000000|((displacement//4)&0xffffff),0xe3a00000,0xe8bd8010)
    patched=raw[:offset]+struct.pack('<IIII',*words)+raw[offset+16:]
    assert len(patched)==len(raw) and patched[:offset]==raw[:offset] and patched[offset+16:]==raw[offset+16:]
    compressed=lzma.compress(patched,format=lzma.FORMAT_XZ,check=lzma.CHECK_CRC32,
        filters=[{'id':lzma.FILTER_ARM},{'id':lzma.FILTER_LZMA2,'preset':9,'dict_size':32*1024*1024,'lc':4}])
    padding=XZ_BYTES-len(compressed)
    if padding<0 or padding%4:raise ValueError('New stream cannot retain the exact zImage layout')
    payload=body[:XZ_OFFSET]+compressed+bytes(padding)+body[XZ_OFFSET+XZ_BYTES:]
    # The ARM decompressor wrapper, address tables, trailing64bytes and all
    # offsets remain identical. XZ_SINGLE stops at the first stream end.
    assert len(payload)==len(body) and payload[:XZ_OFFSET]==body[:XZ_OFFSET]
    assert payload[XZ_OFFSET+XZ_BYTES:]==body[XZ_OFFSET+XZ_BYTES:]
    check=lzma.LZMADecompressor();assert check.decompress(payload[XZ_OFFSET:])==patched and check.eof
    header=bytearray(original[:64]);struct.pack_into('>I',header,24,zlib.crc32(payload));header[4:8]=bytes(4)
    header[32:64]=b'Linux-4.9.129 no-hw-watchdog'.ljust(32,b'\0')
    struct.pack_into('>I',header,4,zlib.crc32(header));image=bytes(header)+payload
    assert firmware_package.check_image(2,image)==image
    expected=json.loads((ROOT/'config/kernel-patch.json').read_text())['kernel_uimage_sha256']
    if hashlib.sha256(image).hexdigest()!=expected:raise ValueError('Generated kernel differs from reviewed release input')
    out=ROOT/'output/kernel';out.mkdir(parents=True,exist_ok=True)
    (out/'kernel.uImage').write_bytes(image);(out/'kernel-decompressed.bin').write_bytes(patched)
    report={'base_uimage_sha256':BASE_SHA,'base_decompressed_sha256':RAW_SHA,'kernel_uimage_sha256':hashlib.sha256(image).hexdigest(),'patched_decompressed_sha256':hashlib.sha256(patched).hexdigest(),'image_bytes':len(image),'decompressed_modified_range':{'address':hex(INIT),'bytes':16,'before':EXPECTED.hex(),'after':struct.pack('<IIII',*words).hex()},'assembly':['push {r4, lr}','bl '+hex(PAUSE)+' (stock board fh_wdt_pause)','mov r0, #0','pop {r4, pc}'],'literal_xrefs':['0xc04bf478'],'next_function':'0xc04bf484 mmc_init, unchanged','behavior':'pause watchdog clock using stock board function; do not register watchdog driver or its ping timer','xz_bytes':len(compressed),'xz_zero_padding':padding,'wrapper_and_tail_unchanged':True,'all_other_decompressed_bytes_unchanged':True,'module_abi_and_kernel_version_unchanged':True,'evidence':['ev_ab1373917f8fa1c51899a6d98c3af84efa7800c67fe214406be559e1db71361b','ev_1e08f87725ac3fa893676ef5f23fb0e986dc97026d266990c5b8aec581db58c4','ev_bec4823c37f27632cd193e484ffe221ce85cf48f72200d4144953b8b097f01d4'],'boot_tested':False,'ready_to_flash':False}
    (out/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Prepared exact watchdog-disabled RAM-test kernel:',len(image),'bytes; changed only16 decompressed bytes')

if __name__=='__main__':main()
