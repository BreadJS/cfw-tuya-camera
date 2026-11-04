#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static unsigned sync_calls,fail_sync;
static int controlled_sync(int fd){if(++sync_calls==fail_sync){errno=EIO;return -1;}return(int)syscall(SYS_fsync,fd);}
#define fsync controlled_sync
#include "../../src/anyka/board_settings.c"
#undef fsync
int main(void)
{
 char path[]="/tmp/ptz-board-settings-XXXXXX";assert(mkdtemp(path));int dir=open(path,O_RDONLY|O_DIRECTORY);assert(dir>=0);
 struct anyka_board_settings s={0};assert(!anyka_board_settings_load(dir,&s));assert(s.day_mode==1&&!s.ir_led&&!s.status_led);
 struct stat st;assert(fstatat(dir,"board.conf",&st,0)<0&&errno==ENOENT);
 s=(struct anyka_board_settings){0,1,1};assert(!anyka_board_settings_save(dir,&s));assert(!fstatat(dir,"board.conf",&st,0)&&(st.st_mode&0777)==0600&&st.st_size==20);
 struct anyka_board_settings out={0};assert(!anyka_board_settings_load(dir,&out)&&!memcmp(&s,&out,sizeof s));
 sync_calls=0;fail_sync=1;struct anyka_board_settings next={1,0,0};assert(anyka_board_settings_save(dir,&next)==-EIO);assert(!anyka_board_settings_load(dir,&out)&&!memcmp(&s,&out,sizeof s));
 sync_calls=0;fail_sync=2;assert(anyka_board_settings_save(dir,&next)==-EIO);assert(!anyka_board_settings_load(dir,&out)&&!memcmp(&next,&out,sizeof next));fail_sync=0;
 int temp=openat(dir,"board.tmp",O_WRONLY|O_CREAT|O_EXCL,0600);assert(temp>=0&&write(temp,"partial",7)==7);close(temp);assert(!anyka_board_settings_save(dir,&s));
 assert(!linkat(dir,"board.conf",dir,"alias",0));assert(anyka_board_settings_load(dir,&out)==-EPROTO);assert(anyka_board_settings_save(dir,&s)==-EPERM);assert(!unlinkat(dir,"alias",0));
 assert(!symlinkat("board.conf",dir,"board.tmp"));assert(anyka_board_settings_save(dir,&s)==-EPROTO);assert(!unlinkat(dir,"board.tmp",0));
 s.day_mode=2;assert(anyka_board_settings_save(dir,&s)==-EINVAL);s.day_mode=0;
 assert(!fchmodat(dir,"board.conf",0644,0));assert(anyka_board_settings_load(dir,&out)==-EPROTO);assert(!fchmodat(dir,"board.conf",0600,0));
 int corrupt=openat(dir,"board.conf",O_WRONLY|O_TRUNC);assert(corrupt>=0&&write(corrupt,"x",1)==1);close(corrupt);out=(struct anyka_board_settings){1,1,0};assert(anyka_board_settings_load(dir,&out)==-EPROTO&&out.day_mode==1&&out.ir_led==1);
 assert(!unlinkat(dir,"board.conf",0));close(dir);assert(!rmdir(path));
 puts("PASS board settings: safe defaults, atomic durable saves, pre/post-rename failures, stale-temp recovery, unsafe-file/range/corruption rejection");
}
