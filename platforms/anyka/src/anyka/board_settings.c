/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "board_settings.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int directory_safe(int dir)
{struct stat st;return fstat(dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)?-EPERM:0;}
static int private_file(const struct stat *st)
{return !S_ISREG(st->st_mode)||st->st_uid!=geteuid()||(st->st_mode&07777)!=0600||st->st_nlink!=1;}
static uint32_t get32(const unsigned char *p)
{return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,unsigned n)
{p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
void anyka_board_settings_defaults(struct anyka_board_settings *out)
{if(out)*out=(struct anyka_board_settings){1,0,0};}
int anyka_board_settings_load(int dir,struct anyka_board_settings *out)
{
    if(!out)return -EINVAL;
    int rc=directory_safe(dir);if(rc)return rc;
    pthread_mutex_lock(&mutex);int fd=openat(dir,"board.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){rc=errno==ENOENT?0:-errno;if(!rc)anyka_board_settings_defaults(out);pthread_mutex_unlock(&mutex);return rc;}
    unsigned char data[20];struct stat st;size_t done=0;
    if(fstat(fd,&st)||private_file(&st)||st.st_size!=sizeof data)rc=-EPROTO;
    while(!rc&&done<sizeof data){ssize_t n=read(fd,data+done,sizeof data-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&(fstat(fd,&st)||private_file(&st)||st.st_size!=sizeof data||memcmp(data,"AKBRD001",8)))rc=-EPROTO;
    if(!rc){struct anyka_board_settings next={get32(data+8),get32(data+12),get32(data+16)};if(next.day_mode>1||next.ir_led>1||next.status_led>1)rc=-EPROTO;else *out=next;}
    if(close(fd)&&!rc)rc=-errno;
    pthread_mutex_unlock(&mutex);return rc;
}
int anyka_board_settings_save(int dir,const struct anyka_board_settings *settings)
{
    if(!settings||settings->day_mode>1||settings->ir_led>1||settings->status_led>1)return -EINVAL;
    int rc=directory_safe(dir);if(rc)return rc;
    pthread_mutex_lock(&mutex);struct stat st;
    if(!fstatat(dir,"board.conf",&st,AT_SYMLINK_NOFOLLOW)){if(private_file(&st))rc=-EPERM;}else if(errno!=ENOENT)rc=-errno;
    if(!rc&&!fstatat(dir,"board.tmp",&st,AT_SYMLINK_NOFOLLOW)){if(private_file(&st)||st.st_size>20)rc=-EPROTO;else if(unlinkat(dir,"board.tmp",0))rc=-errno;}else if(!rc&&errno!=ENOENT)rc=-errno;
    unsigned char data[20]={0};memcpy(data,"AKBRD001",8);put32(data+8,settings->day_mode);put32(data+12,settings->ir_led);put32(data+16,settings->status_led);
    int fd=rc?-1:openat(dir,"board.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(!rc&&fd<0)rc=-errno;
    if(fd>=0){size_t done=0;while(done<sizeof data){ssize_t n=write(fd,data+done,sizeof data-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
        if(!rc&&fsync(fd))rc=-errno;
        if(close(fd)&&!rc)rc=-errno;
        if(!rc&&renameat(dir,"board.tmp",dir,"board.conf"))rc=-errno;
        if(!rc&&fsync(dir))rc=-errno;
        if(rc)unlinkat(dir,"board.tmp",0);
    }
    pthread_mutex_unlock(&mutex);return rc;
}
