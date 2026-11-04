#!/usr/bin/env python3
"""Exercise actual early-root normalization with fake metadata/mount callbacks."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
static int mounted, calls, broken, unchanged, metadata_error;
static unsigned long magic=0x73717368UL;
static int fake_vfs(const char *path, struct statvfs *s) {
 assert(!strcmp(path,"/")); if(metadata_error){errno=EIO;return -1;}
 memset(s,0,sizeof *s);s->f_flag=mounted?ST_RDONLY:0;return 0;
}
static int fake_fs(const char *path,struct statfs *s) {
 assert(!strcmp(path,"/"));memset(s,0,sizeof *s);s->f_type=magic;return 0;
}
static int fake_mount(const char *src,const char *target,const char *type,unsigned long flags,const void *data) {
 assert(!src&&!strcmp(target,"/")&&!type&&!data);
 assert(flags==(MS_REMOUNT|MS_RDONLY));calls++;
 if(broken){errno=EPERM;return -1;}if(!unchanged)mounted=1;return 0;
}
#define statvfs(...) fake_vfs(__VA_ARGS__)
#define statfs(...) fake_fs(__VA_ARGS__)
#define mount(...) fake_mount(__VA_ARGS__)
#define RAMINIT_HOST_TEST
#define main archive_main
#include "raminit.c"
#undef main
int main(void) {
 mounted=1;assert(source_readonly()==0&&calls==0);
 mounted=0;metadata_error=1;assert(source_readonly()==-1&&errno==EIO&&calls==0);metadata_error=0;
#ifdef CAMERA_NOR_RAM_ROOT
 assert(source_readonly()==0&&mounted&&calls==1);
 mounted=0;calls=0;magic=0x4d44;assert(source_readonly()==-1&&errno==EINVAL&&calls==0);magic=0x73717368;
 broken=1;assert(source_readonly()==-1&&errno==EPERM&&calls==1);broken=0;calls=0;
 unchanged=1;assert(source_readonly()==-1&&errno==EROFS&&calls==1);
#else
 assert(source_readonly()==-1&&errno==EROFS&&calls==0);
 (void)fake_fs;(void)fake_mount;
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='camera-source-readonly-') as temp:
    temp = Path(temp)
    driver = temp / 'driver.c'
    driver.write_text(DRIVER)
    # Archive parsing is discarded by the linker in this focused test.
    (temp / 'ramroot_expected.h').write_text('#define RAMROOT_BYTES 1\nstatic const unsigned char ramroot_hash[64]={0};\n')
    for nor in (False, True):
        binary = temp / ('nor' if nor else 'fat')
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        *(['-DCAMERA_NOR_RAM_ROOT'] if nor else []),
                        '-I' + str(ROOT / 'ram-boot'),
                        '-I' + str(temp),
                        '-I' + str(ROOT / 'src/ota/vendor'), str(driver),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('PASS: NOR read-only normalization/recheck/errors; FAT refuses writable mounts; ASan/UBSan')
