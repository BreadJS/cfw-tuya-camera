#define _GNU_SOURCE
#include "experiment_owner.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static const char lock_path[]="/tmp/camera-native-owner.lock";
static const char *const devices[]={"/dev/vmm_userdev","/dev/isp","/dev/pae",
    "/dev/hevc","/dev/media_process","/dev/jpeg","/dev/bgm","/dev/fh_audio","/dev/mem"};
struct device { dev_t dev,rdev;ino_t inode; };
static int number(const char *s)
{
    if(!*s)return 0;
    for(;*s;s++)if(*s<'0'||*s>'9')return 0;
    return 1;
}
int fh_experiment_foreign_users(void)
{
    struct device dev[sizeof devices/sizeof *devices];size_t count=0;
    for(size_t i=0;i<sizeof devices/sizeof *devices;i++) {
        struct stat st;
        if(stat(devices[i],&st)) {if(errno==ENOENT)continue;return -errno;}
        if(!S_ISCHR(st.st_mode))return -ENODEV;
        dev[count++]=(struct device){st.st_dev,st.st_rdev,st.st_ino};
    }
    if(count<4)return -ENODEV;
    DIR *proc=opendir("/proc");if(!proc)return -errno;
    struct dirent *ent;int result=0;
    while(!result && (ent=readdir(proc))) {
        if(!number(ent->d_name))continue;
        long pid=strtol(ent->d_name,NULL,10);if(pid==getpid())continue;
        char path[96],exe[512];
        snprintf(path,sizeof path,"/proc/%ld/cmdline",pid);
        int command=open(path,O_RDONLY|O_CLOEXEC);
        if(command>=0) {
            char text[1024];ssize_t bytes=read(command,text,sizeof text-1);close(command);
            if(bytes<0) {result=-errno;break;}
            for(ssize_t i=0;i<bytes;i++)if(!text[i])text[i]=' ';
            text[bytes]=0;
            if(strstr(text,"/usr/init/app_init.sh")||strstr(text,"/tmp/custom.sh")||
               strstr(text,"/mnt/sdcard/custom.sh")) {result=-EBUSY;break;}
        } else if(errno!=ENOENT && errno!=ESRCH) {result=-errno;break;}
        snprintf(path,sizeof path,"/proc/%ld/exe",pid);
        ssize_t n=readlink(path,exe,sizeof exe-1);
        if(n>=0) {
            exe[n]=0;
            /* Launchers must be stopped too, before claiming the hardware. */
            if(!strcmp(exe,"/usr/bin/dgiot") || !strcmp(exe,"/usr/bin/daemon")) {
                result=-EBUSY;break;
            }
        } else if(errno!=ENOENT && errno!=ESRCH) {result=-errno;break;}
        snprintf(path,sizeof path,"/proc/%ld/fd",pid);
        DIR *fds=opendir(path);
        if(!fds) {if(errno==ENOENT||errno==ESRCH)continue;result=-errno;break;}
        struct dirent *fd;
        while(!result && (fd=readdir(fds))) {
            if(!number(fd->d_name))continue;
            char name[128];snprintf(name,sizeof name,"/proc/%ld/fd/%.20s",pid,fd->d_name);
            struct stat st;
            if(stat(name,&st)) {if(errno==ENOENT||errno==ESRCH)continue;result=-errno;break;}
            if(S_ISCHR(st.st_mode))for(size_t i=0;i<count;i++)
                if(st.st_rdev==dev[i].rdev) {result=-EBUSY;break;}
        }
        closedir(fds);
        if(result)break;
        snprintf(path,sizeof path,"/proc/%ld/maps",pid);
        FILE *maps=fopen(path,"r");
        if(!maps) {if(errno==ENOENT||errno==ESRCH)continue;result=-errno;break;}
        char line[1024];
        while(!result && fgets(line,sizeof line,maps)) {
            unsigned maj,min;unsigned long long inode;
            if(sscanf(line,"%*s %*s %*s %x:%x %llu",&maj,&min,&inode)!=3)continue;
            for(size_t i=0;i<count;i++)if(makedev(maj,min)==dev[i].dev && inode==dev[i].inode) {
                result=-EBUSY;break;
            }
        }
        if(ferror(maps))result=-EIO;
        fclose(maps);
    }
    closedir(proc);return result;
}
int fh_experiment_owner_check(void *opaque)
{
    struct fh_experiment_owner *s=opaque;struct stat fd,path;
    if(!s||s->lock_fd<0||s->pid!=getpid()||!s->references)return -EPERM;
    if(fstat(s->lock_fd,&fd)||lstat(lock_path,&path))return -errno;
    if(!S_ISREG(path.st_mode)||path.st_nlink!=1||path.st_uid!=geteuid()||
       (path.st_mode&077)||fd.st_dev!=path.st_dev||fd.st_ino!=path.st_ino||
       (unsigned long long)fd.st_dev!=s->lock_device||
       (unsigned long long)fd.st_ino!=s->lock_inode)return -ESTALE;
    return flock(s->lock_fd,LOCK_EX|LOCK_NB)?-errno:0;
}
int fh_experiment_owner_open(struct fh_experiment_owner *s)
{
    if(!s||geteuid()!=0)return -EPERM;
    *s=(struct fh_experiment_owner){.lock_fd=-1};
    struct statfs parent;
    if(statfs("/tmp",&parent))return -errno;
    if(parent.f_type!=0x858458f6 && parent.f_type!=0x01021994)return -EPERM;
    int fd=open(lock_path,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    struct stat st;struct statfs fs;int rc=0;
    if(fstat(fd,&st)||fstatfs(fd,&fs)||flock(fd,LOCK_EX|LOCK_NB))rc=-errno;
    else if(!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||
            (st.st_mode&077)||(fs.f_type!=0x858458f6 && fs.f_type!=0x01021994))rc=-EPERM;
    if(!rc)rc=fh_experiment_foreign_users();
    if(rc) {close(fd);unlink(lock_path);return rc;}
    *s=(struct fh_experiment_owner){fd,getpid(),1,
        (unsigned long long)st.st_dev,(unsigned long long)st.st_ino};
    return fh_experiment_owner_check(s);
}
int fh_experiment_owner_retain(void *opaque)
{
    struct fh_experiment_owner *s=opaque;int rc=fh_experiment_owner_check(s);
    if(rc)return rc;
    if(s->references==UINT_MAX)return -EOVERFLOW;
    ++s->references;return 0;
}
int fh_experiment_owner_release(void *opaque)
{
    struct fh_experiment_owner *s=opaque;int rc=fh_experiment_owner_check(s);
    if(rc)return rc;
    if(s->references<=1)return -EBUSY;
    --s->references;return 0;
}
int fh_experiment_owner_quiesce(void *opaque)
{
    int rc=fh_experiment_owner_check(opaque);
    return rc?rc:-ENOTSUP;
}
