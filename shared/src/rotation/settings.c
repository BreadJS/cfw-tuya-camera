#define _GNU_SOURCE
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static atomic_uint serial;
static int directory(int fd){struct stat s;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-1:0;}
static int private_file(const struct stat *s){return !S_ISREG(s->st_mode)||s->st_uid!=geteuid()||(s->st_mode&07777)!=0600||s->st_nlink!=1;}
int camera_rotation_load(int dir,unsigned *out)
{
 if(!out||directory(dir))return -1;
 int fd=openat(dir,"rotation.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(fd<0){if(errno!=ENOENT)return -1;*out=0;return 0;}
 unsigned char b[12];struct stat before,after;int rc=-1;
 if(fstat(fd,&before)||private_file(&before)||before.st_size!=sizeof b)goto done;
 size_t at=0;while(at<sizeof b){ssize_t n=read(fd,b+at,sizeof b-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
 if(fstat(fd,&after)||private_file(&after)||after.st_size!=before.st_size||
  after.st_mtim.tv_sec!=before.st_mtim.tv_sec||after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec||
  after.st_ctim.tv_sec!=before.st_ctim.tv_sec||after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec||
  memcmp(b,"CAMROT1\0",8)||b[8]>1||b[9]||b[10]||b[11])goto done;
 if(close(fd))return -1;
 *out=b[8];return 0;
done:close(fd);return rc;
}
int camera_rotation_save(int dir,unsigned enabled)
{
 if(enabled>1||directory(dir))return -1;
 struct stat old;if(!fstatat(dir,"rotation.conf",&old,AT_SYMLINK_NOFOLLOW)){if(private_file(&old))return -1;}else if(errno!=ENOENT)return -1;
 char name[80];int fd=-1;
 for(unsigned i=0;i<100;i++){
  unsigned next=atomic_fetch_add(&serial,1)+1;snprintf(name,sizeof name,"rotation.tmp.%ld.%u",(long)getpid(),next);
  fd=openat(dir,name,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd>=0)break;if(errno!=EEXIST)return -1;
 }
 if(fd<0)return -1;
 unsigned char b[12]="CAMROT1";b[8]=(unsigned char)enabled;
 int rc=-1;size_t at=0;
 if(fchmod(fd,0600))goto done;
 while(at<sizeof b){ssize_t n=write(fd,b+at,sizeof b-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
 if(fsync(fd))goto done;
 if(close(fd)){fd=-1;goto done;}fd=-1;
 if(renameat(dir,name,dir,"rotation.conf")||fsync(dir))goto done;
 rc=0;
done:if(fd>=0)close(fd);if(rc)unlinkat(dir,name,0);return rc;
}
