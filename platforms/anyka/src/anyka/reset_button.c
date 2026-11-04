/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "reset_button.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct event32 {int32_t seconds,microseconds;uint16_t type,code;int32_t value;};
_Static_assert(sizeof(struct event32)==16,"ARM32 evdev record");
struct reset_reader {int fd;void (*callback)(void *);void *opaque;};
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u;}
static int update_hold(const struct event32 *event,uint64_t now,uint64_t *pressed,int *held){if(event->type==0&&event->code==3){*held=0;return 0;}if(event->type!=1||event->code!=CAMERA_RESET_EVENT_CODE)return 0;if(event->value==1){if(!*held){*pressed=now;*held=1;}return 0;}if(event->value==0){int reset=*held&&now>=*pressed&&now-*pressed>=CAMERA_RESET_HOLD_SECONDS*1000u;*held=0;return reset;}return 0;}
static void *reader(void *opaque){struct reset_reader *r=opaque;uint64_t pressed=0;int held=0;for(;;){struct pollfd p={r->fd,POLLIN,0};int rc=poll(&p,1,250);if(rc<0&&errno==EINTR)continue;if(rc<0||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))break;if(!(p.revents&POLLIN))continue;struct event32 events[16];ssize_t n=read(r->fd,events,sizeof events);if(n<0&&(errno==EINTR||errno==EAGAIN))continue;if(n<=0||n%(ssize_t)sizeof events[0])break;for(size_t i=0;i<(size_t)n/sizeof events[0];i++)if(update_hold(events+i,now_ms(),&pressed,&held))r->callback(r->opaque);}close(r->fd);free(r);return NULL;}
static int gpio_attribute(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;size_t size=strlen(text);ssize_t n;do{n=write(fd,text,size);}while(n<0&&errno==EINTR);int rc=n<0?-errno:(size_t)n==size?0:-EIO;close(fd);return rc;}
static void *gpio_reader(void *opaque){struct reset_reader *r=opaque;uint64_t pressed=0,changed=now_ms();int held=0,previous=-1,stable=-1,armed=0;for(;;){char value;if(pread(r->fd,&value,1,0)!=1||(value!='0'&&value!='1'))break;int level=value-'0';uint64_t now=now_ms();if(level!=previous){previous=level;changed=now;}if(now-changed>=50&&stable!=level){stable=level;if(level!=CAMERA_RESET_ON_LEVEL){if(armed&&held){struct event32 event={.type=1,.code=CAMERA_RESET_EVENT_CODE,.value=0};if(update_hold(&event,now,&pressed,&held))r->callback(r->opaque);}armed=1;}else if(armed){struct event32 event={.type=1,.code=CAMERA_RESET_EVENT_CODE,.value=1};(void)update_hold(&event,now,&pressed,&held);}}struct timespec pause={0,20000000};while(nanosleep(&pause,&pause)&&errno==EINTR){}}close(r->fd);free(r);return NULL;}
int anyka_reset_start(void (*callback)(void *),void *opaque){
 if(!callback)return -EINVAL;
 if(!CAMERA_HAS_RESET)return 0;
 int fd;
 if(CAMERA_RESET_EVDEV)fd=open(CAMERA_RESET_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
 else{int rc=gpio_attribute("/sys/class/gpio/export",CAMERA_RESET_EXPORT);if(rc&&rc!=-EBUSY)return rc;rc=gpio_attribute(CAMERA_RESET_DIRECTION,"in");if(rc)return rc;rc=gpio_attribute(CAMERA_RESET_ACTIVE_LOW,"0");if(rc)return rc;fd=open(CAMERA_RESET_VALUE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);}
 if(fd<0)return -errno;
 struct reset_reader *r=malloc(sizeof *r);if(!r){close(fd);return -ENOMEM;}*r=(struct reset_reader){fd,callback,opaque};pthread_t thread;int rc=pthread_create(&thread,NULL,CAMERA_RESET_EVDEV?reader:gpio_reader,r);if(rc){close(fd);free(r);return -rc;}pthread_detach(thread);return 0;
}
static int private_dir(int fd){struct stat s;if(fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&0077))return -EPERM;return 0;}
static int clear_directory(int fd,unsigned depth){if(depth>3)return -ELOOP;int copy=dup(fd);if(copy<0)return -errno;DIR *d=fdopendir(copy);if(!d){close(copy);return -errno;}int rc=0;struct dirent *entry;while((entry=readdir(d))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;struct stat s;if(fstatat(fd,entry->d_name,&s,AT_SYMLINK_NOFOLLOW)){rc=-errno;break;}if(S_ISDIR(s.st_mode)){int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(child<0){rc=-errno;break;}rc=clear_directory(child,depth+1);close(child);if(!rc&&unlinkat(fd,entry->d_name,AT_REMOVEDIR))rc=-errno;}else if(S_ISREG(s.st_mode)){if(unlinkat(fd,entry->d_name,0))rc=-errno;}else rc=-EPERM;if(rc)break;}closedir(d);if(!rc&&fsync(fd))rc=-errno;return rc;}
int anyka_reset_settings(int dir){int rc=private_dir(dir);if(rc)return rc;static const char *files[]={"credentials.conf","display.conf","rotation.conf","anyka-picture.conf","anyka-picture.tmp","board.conf","video-mode.conf"};for(unsigned i=0;i<sizeof files/sizeof files[0];i++){struct stat s;if(fstatat(dir,files[i],&s,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT)continue;return -errno;}if(!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||s.st_nlink!=1)return -EPERM;if(unlinkat(dir,files[i],0))return -errno;}int wifi=openat(dir,"wifi",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(wifi>=0){rc=clear_directory(wifi,0);close(wifi);if(rc)return rc;}else if(errno!=ENOENT)return -errno;if(fsync(dir))return -errno;
struct stat marker;if(!fstatat(dir,"reset.pending",&marker,AT_SYMLINK_NOFOLLOW)){if(!S_ISREG(marker.st_mode)||marker.st_uid!=geteuid()||marker.st_nlink!=1)return -EPERM;if(unlinkat(dir,"reset.pending",0))return -errno;}else if(errno!=ENOENT)return -errno;
return fsync(dir)?-errno:0;}
