/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "video_mode.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static atomic_uint serial;
static int directory(int fd)
{struct stat s;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-EPERM:0;}
static int private_file(const struct stat *s)
{return !S_ISREG(s->st_mode)||s->st_uid!=geteuid()||(s->st_mode&07777)!=0600||s->st_nlink!=1;}
void anyka_video_mode_defaults(struct anyka_video_mode *out)
{if(out)*out=(struct anyka_video_mode){1920,1080,15,ANYKA_VIDEO_PROFILE_MAIN};}
int anyka_video_mode_validate(const struct anyka_video_mode *m)
{
    return !m||!((m->width==1920&&m->height==1080)||(m->width==1280&&m->height==720))||
        (m->fps!=15&&m->fps!=25)||(m->profile!=ANYKA_VIDEO_PROFILE_MAIN&&m->profile!=ANYKA_VIDEO_PROFILE_BASELINE)?-EINVAL:0;
}
static unsigned le16(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
int anyka_video_mode_load(int dir,struct anyka_video_mode *out)
{
    if(!out)return -EINVAL;
    int rc=directory(dir);if(rc)return rc;
    int fd=openat(dir,"video-mode.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT)return -errno;anyka_video_mode_defaults(out);return 0;}
    unsigned char bytes[16];struct stat before,after;rc=-EIO;
    if(fstat(fd,&before)||private_file(&before)||before.st_size!=sizeof bytes){rc=-EPERM;goto done;}
    size_t at=0;while(at<sizeof bytes){ssize_t n=read(fd,bytes+at,sizeof bytes-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
    if(fstat(fd,&after)||private_file(&after)||after.st_size!=before.st_size||
       after.st_mtim.tv_sec!=before.st_mtim.tv_sec||after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec||
       after.st_ctim.tv_sec!=before.st_ctim.tv_sec||after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec){rc=-EAGAIN;goto done;}
    if(memcmp(bytes,"AKVMOD1\0",8)||bytes[14]||bytes[15]){rc=-EINVAL;goto done;}
    struct anyka_video_mode proposed={le16(bytes+8),le16(bytes+10),bytes[12],bytes[13]};
    rc=anyka_video_mode_validate(&proposed);if(rc)goto done;
    if(close(fd))return -errno;
    *out=proposed;return 0;
done:close(fd);return rc;
}
int anyka_video_mode_save(int dir,const struct anyka_video_mode *mode)
{
    int rc=anyka_video_mode_validate(mode);if(rc)return rc;
    rc=directory(dir);if(rc)return rc;
    struct stat old;
    if(!fstatat(dir,"video-mode.conf",&old,AT_SYMLINK_NOFOLLOW)){if(private_file(&old))return -EPERM;}
    else if(errno!=ENOENT)return -errno;
    char name[80];int fd=-1;
    for(unsigned attempt=0;attempt<100;attempt++){
        unsigned next=atomic_fetch_add(&serial,1)+1;
        snprintf(name,sizeof name,"video-mode.tmp.%ld.%u",(long)getpid(),next);
        fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0)break;
        if(errno!=EEXIST)return -errno;
    }
    if(fd<0)return -EEXIST;
    unsigned char bytes[16]="AKVMOD1";
    bytes[8]=(unsigned char)mode->width;bytes[9]=(unsigned char)(mode->width>>8);
    bytes[10]=(unsigned char)mode->height;bytes[11]=(unsigned char)(mode->height>>8);
    bytes[12]=(unsigned char)mode->fps;bytes[13]=(unsigned char)mode->profile;
    rc=0;size_t at=0;
    if(fchmod(fd,0600)){rc=-errno;goto done;}
    while(at<sizeof bytes){ssize_t n=write(fd,bytes+at,sizeof bytes-at);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;goto done;}at+=(size_t)n;}
    if(fsync(fd)){rc=-errno;goto done;}
    if(close(fd)){fd=-1;rc=-errno;goto done;}fd=-1;
    if(renameat(dir,name,dir,"video-mode.conf")||fsync(dir))rc=-errno;
 done:if(fd>=0)close(fd);if(rc)unlinkat(dir,name,0);return rc;
}
