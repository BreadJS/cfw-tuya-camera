/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "application_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <time.h>
static int ram_path(const char *path)
{struct statfs fs;if(statfs(path,&fs))return -errno;return(unsigned long)fs.f_type==0x01021994UL||(unsigned long)fs.f_type==0x858458f6UL?0:-EPERM;}
static int preserved_library_metadata(const char *canonical,const struct stat *st,const struct statfs *fs)
{
    /* APP alone is replaced. ROOTFS remains immutable and the installer pins
     * its complete digest before erase. Original SquashFS libraries carry
     * POSIX write bits and the reviewed build UID1000. Accept that exact
     * packaging UID as well as root; effective readonly filesystem and the
     * fixed partition identity remain mandatory. */
    return !strncmp(canonical,"/lib/",5)&&S_ISREG(st->st_mode)&&(st->st_uid==0||st->st_uid==1000)&&
        major(st->st_dev)==31&&minor(st->st_dev)==5&&
        (unsigned long)fs->f_type==0x73717368UL&&(fs->f_flags&ST_RDONLY);
}
static int preserved_library(const char *path,const struct stat *st)
{
    char canonical[PATH_MAX];struct statfs fs;
    if(!realpath(path,canonical)||statfs(canonical,&fs))return 0;
    return preserved_library_metadata(canonical,st,&fs);
}
int anyka_application_ram_safe(void)
{
    if(geteuid()!=0)return -EPERM;
    int rc=ram_path(".");if(rc)return rc;
    rc=ram_path("/proc/self/exe");if(rc)return rc;
    FILE *maps=fopen("/proc/self/maps","re");if(!maps)return -errno;
    char line[2048];int result=0;
    while(fgets(line,sizeof line,maps)) {
        if(!strchr(line,'\n')){result=-EOVERFLOW;break;}
        unsigned long start,end,offset,inode;unsigned major,minor;char permissions[5];int used=0;
        if(sscanf(line,"%lx-%lx %4s %lx %x:%x %lu %n",&start,&end,permissions,&offset,&major,&minor,&inode,&used)!=7){result=-EPROTO;break;}
        char *path=line+used;while(*path==' ')path++;path[strcspn(path,"\n")]=0;
        if(*path!='/')continue;
        if(strchr(path,'\\')||strstr(path," (deleted)")){result=-EPERM;break;}
        struct stat st;if(stat(path,&st)){result=-errno;break;}
        if(S_ISREG(st.st_mode)){result=ram_path(path);if(result&&preserved_library(path,&st))result=0;if(result)break;}
        else if(!S_ISCHR(st.st_mode)){result=-EPERM;break;}
    }
    if(ferror(maps)&&!result)result=-EIO;
    fclose(maps);return result;
}
static int app_mount_metadata(const struct stat *st,const struct statfs *fs)
{
    if(!S_ISDIR(st->st_mode)||st->st_uid||(unsigned long)fs->f_type!=0x73717368UL||!(fs->f_flags&ST_RDONLY))return 0;
    if(major(st->st_dev)==31&&minor(st->st_dev)==7)return 1;
    return major(st->st_dev)==7?2:0;
}
static int same_file_bytes(const char *running,const char *mounted,size_t limit)
{
    int a=open(running,O_RDONLY|O_CLOEXEC),b=open(mounted,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    struct stat x,y;int result=-1;
    if(a<0||b<0||fstat(a,&x)||fstat(b,&y)||!S_ISREG(x.st_mode)||!S_ISREG(y.st_mode)||
       x.st_uid||y.st_uid||x.st_size<1||x.st_size!=y.st_size||(uint64_t)x.st_size>limit)goto done;
    char left[4096],right[4096];
    for(off_t offset=0;offset<x.st_size;){
        size_t count=(uint64_t)(x.st_size-offset)<sizeof left?(size_t)(x.st_size-offset):sizeof left;
        size_t n=0;while(n<count){ssize_t got=pread(a,left+n,count-n,offset+(off_t)n);if(got<0&&errno==EINTR)continue;if(got<=0)goto done;n+=(size_t)got;}
        n=0;while(n<count){ssize_t got=pread(b,right+n,count-n,offset+(off_t)n);if(got<0&&errno==EINTR)continue;if(got<=0)goto done;n+=(size_t)got;}
        if(memcmp(left,right,count))goto done;
        offset+=(off_t)count;
    }
    result=0;
done:if(a>=0)close(a);if(b>=0)close(b);return result;
}
int anyka_application_app_source(void)
{
    if(anyka_application_ram_safe())return 0;
    struct stat st;struct statfs fs;
    if(stat("/usr",&st)||statfs("/usr",&fs))return 0;
    int source=app_mount_metadata(&st,&fs);if(!source)return 0;
    if(source==2){
        char sys[128],backing[PATH_MAX];snprintf(sys,sizeof sys,"/sys/dev/block/%u:%u/loop/backing_file",major(st.st_dev),minor(st.st_dev));
        FILE *file=fopen(sys,"re");if(!file)return 0;
        int valid=fgets(backing,sizeof backing,file)!=NULL;fclose(file);
        if(!valid||!strchr(backing,'\n'))return 0;
        backing[strcspn(backing,"\n")]=0;
        if(backing[0]!='/'||strstr(backing," (deleted)")||ram_path(backing))return 0;
    }
    if(same_file_bytes("/proc/self/exe","/usr/libexec/camera/sdk-host",6u*1024u*1024u))return 0;
    FILE *maps=fopen("/proc/self/maps","re");if(!maps)return 0;
    uintptr_t address=(uintptr_t)&anyka_application_app_source;char line[2048];int matched=0;
    while(fgets(line,sizeof line,maps)){
        unsigned long begin,end,offset,inode;unsigned device_major,device_minor;char permissions[5];int used=0;
        if(!strchr(line,'\n')||sscanf(line,"%lx-%lx %4s %lx %x:%x %lu %n",&begin,&end,permissions,&offset,&device_major,&device_minor,&inode,&used)!=7)break;
        if(address<begin||address>=end)continue;
        char *path=line+used;while(*path==' ')path++;path[strcspn(path,"\n")]=0;
        if(*path=='/'&&!stat(path,&st)&&st.st_ino==inode&&major(st.st_dev)==device_major&&minor(st.st_dev)==device_minor&&
           !same_file_bytes(path,"/usr/libexec/camera/camera-application.so",2u*1024u*1024u))matched=1;
        break;
    }
    fclose(maps);return matched?source:0;
}
int anyka_application_source_receipt(const char *directory,int source)
{
    if(!directory||source<0||source>2||geteuid())return -EPERM;
    if(mkdir(directory,0700)&&errno!=EEXIST)return -errno;
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return -errno;
    struct stat st;struct statfs fs;
    if(fstat(dir,&st)||st.st_uid||(st.st_mode&077)||fstatfs(dir,&fs)||
       ((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)){close(dir);return -EPERM;}
    /* Invalidate a prior process's marker before publishing this owner. */
    if(unlinkat(dir,"app-source",0)&&errno!=ENOENT){int rc=-errno;close(dir);return rc;}
    const char *text=source==1?"APP-mtd7\n":source==2?"RAM-APP-overlay\n":"RAM-files-only\n";
    int fd=openat(dir,"app-source.tmp",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);int rc=0;
    if(fd<0){rc=-errno;close(dir);return rc;}
    size_t bytes=strlen(text),done=0;
    while(done<bytes){ssize_t n=write(fd,text+done,bytes-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&(fchmod(fd,0600)||fsync(fd)))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(dir,"app-source.tmp",dir,"app-source"))rc=-errno;
    if(!rc&&fsync(dir))rc=-errno;
    if(rc)unlinkat(dir,"app-source.tmp",0);
    close(dir);return rc;
}
static int boot_uuid(char out[37])
{
    int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    char text[38];ssize_t n=read(fd,text,sizeof text);close(fd);if(n!=37||text[36]!='\n')return -EPROTO;
    for(unsigned i=0;i<36;i++){if(i==8||i==13||i==18||i==23){if(text[i]!='-')return -EPROTO;}else if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return -EPROTO;}
    memcpy(out,text,37);return 0;
}
static int atomic_uuid(int directory,const char *temporary,const char *name,const char uuid[37])
{
    int fd=openat(directory,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -errno;
    int rc=0;size_t done=0;
    while(done<37){ssize_t n=write(fd,uuid+done,37-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(directory,temporary,directory,name))rc=-errno;
    if(!rc&&fsync(directory))rc=-errno;
    if(rc)unlinkat(directory,temporary,0);
    return rc;
}
static int network_quiesce(const char uuid[37])
{
    const char *path=getenv("CAMERA_NETWORK_RUNTIME");if(!path||!*path)path="/var/run/camera-network";
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return -errno;
    struct stat st;struct statfs fs;int rc=0;
    if(fstat(dir,&st)||st.st_uid||(st.st_mode&077)||fstatfs(dir,&fs)||
       ((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)){close(dir);return -EPERM;}
    int fd=openat(dir,"supervisor.pid",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char text[32];ssize_t bytes=-1;
    if(fd>=0){if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&!st.st_uid&&!(st.st_mode&022))bytes=read(fd,text,sizeof text-1);close(fd);}
    if(bytes<2){close(dir);return -ESRCH;}text[bytes]=0;char *end;errno=0;long pid=strtol(text,&end,10);
    if(errno||pid<=1||pid>INT_MAX||strcmp(end,"\n")||kill((pid_t)pid,0)){close(dir);return -ESRCH;}
    if(unlinkat(dir,"quiesced.ack",0)&&errno!=ENOENT){rc=-errno;close(dir);return rc;}
    rc=atomic_uuid(dir,"quiesced.request.tmp","quiesced",uuid);
    if(rc){close(dir);return rc;}
    for(unsigned attempt=0;attempt<300;attempt++){
        fd=openat(dir,"quiesced.ack",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char ack[38];bytes=-1;
        if(fd>=0){if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&!st.st_uid&&st.st_nlink==1&&!(st.st_mode&022)&&st.st_size==37)bytes=read(fd,ack,sizeof ack);close(fd);}
        if(bytes==37&&!memcmp(ack,uuid,37)){close(dir);return 0;}
        if(kill((pid_t)pid,0)){close(dir);return -ESRCH;}
        struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    }
    close(dir);return -ETIMEDOUT;
}
int anyka_application_quiesce_receipt(const char *directory)
{
    if(!directory||geteuid()!=0)return -EPERM;
    int rc=anyka_application_ram_safe();if(rc)return rc;
    if(mkdir(directory,0700)&&errno!=EEXIST)return -errno;
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return -errno;
    struct stat st;struct statfs fs;
    if(fstat(dir,&st)||st.st_uid||(st.st_mode&077)||fstatfs(dir,&fs)||((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)){close(dir);return -EPERM;}
    char uuid[37];rc=boot_uuid(uuid);if(rc){close(dir);return rc;}
    rc=network_quiesce(uuid);if(rc){close(dir);return rc;}
    /* A previous receipt is replaced atomically; no stale receipt is reused. */
    int fd=openat(dir,"quiesced.tmp",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){close(dir);return -errno;}
    size_t done=0;while(done<sizeof uuid){ssize_t n=write(fd,uuid+done,sizeof uuid-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(dir,"quiesced.tmp",dir,"quiesced"))rc=-errno;
    if(!rc&&fsync(dir))rc=-errno;
    if(rc)unlinkat(dir,"quiesced.tmp",0);
    close(dir);return rc;
}
