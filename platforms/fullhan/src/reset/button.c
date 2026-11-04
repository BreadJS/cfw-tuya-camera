#define _GNU_SOURCE
#include "button.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>
void camera_reset_button_init(struct camera_reset_button *s)
{memset(s,0,sizeof *s);s->candidate=s->stable=-1;}
int camera_reset_button_sample(struct camera_reset_button *s,int level,uint64_t now,int busy)
{
    if(busy||level<0||level>1||(s->seen&&(now<s->last||now-s->last>500))){
        camera_reset_button_init(s);s->seen=1;s->last=now;return 0;
    }
    s->last=now;s->seen=1;
    if(s->candidate!=level){s->candidate=level;s->changed=now;return 0;}
    if(now-s->changed<50)return 0;
    if(s->stable!=level){
        s->stable=level;
        if(!level&&s->armed){s->pressed=1;s->pressed_at=now;}
        if(level&&s->pressed){
            int fire=s->changed-s->pressed_at>=CAMERA_RESET_HOLD_MS;
            s->pressed=0;if(fire){s->armed=0;return 1;}
        }
    }
    if(level&&!s->armed&&now-s->changed>=200)s->armed=1;
    return 0;
}
static int sysfs(int fd)
{
    struct stat st;struct statfs fs;
    if(fstat(fd,&st)||fstatfs(fd,&fs))return -errno;
    return S_ISREG(st.st_mode)&&st.st_uid==0&&(unsigned long)fs.f_type==0x62656572UL?0:-EPERM;
}
static int put(const char *path,const char *text)
{
    int fd=open(path,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -errno;
    int rc=sysfs(fd);size_t size=strlen(text);
    if(!rc){ssize_t n;do{n=write(fd,text,size);}while(n<0&&errno==EINTR);rc=n<0?-errno:(size_t)n==size?0:-EIO;}
    if(close(fd)&&!rc)rc=-errno;
    return rc;
}
static int attribute(const char *path,char *text,size_t size)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -errno;
    int rc=sysfs(fd);ssize_t n=-1;
    if(!rc){do{n=read(fd,text,size-1);}while(n<0&&errno==EINTR);if(n<0)rc=-errno;else text[n]=0;}
    if(close(fd)&&!rc)rc=-errno;
    return rc;
}
int camera_reset_button_gpio_open(void)
{
    struct utsname u;
    if(geteuid()!=0||uname(&u)||strcmp(u.release,"4.9.129")||
       strcmp(u.version,"#55 PREEMPT Sat Dec 11 11:44:24 CST 2021")||
       strncmp(u.machine,"armv6",5))return -ENOTSUP;
    int rc=put("/sys/class/gpio/export",CAMERA_RESET_EXPORT);
    if(rc&&rc!=-EBUSY)return rc;
    if(!rc){rc=put(CAMERA_RESET_DIRECTION,"in");if(rc)return rc;}
    char text[16];rc=attribute(CAMERA_RESET_DIRECTION,text,sizeof text);
    if(rc)return rc;
    if(strcmp(text,"in\n")&&strcmp(text,"in"))return -EPERM;
    rc=attribute(CAMERA_RESET_ACTIVE_LOW,text,sizeof text);
    if(rc)return rc;
    if(strcmp(text,"0\n")&&strcmp(text,"0"))return -ENOTSUP;
    int fd=open(CAMERA_RESET_VALUE,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -errno;
    rc=sysfs(fd);if(rc){close(fd);return rc;}return fd;
}
int camera_reset_button_gpio_read(int fd)
{
    if(lseek(fd,0,SEEK_SET)<0)return -errno;
    char data[4];ssize_t n;do{n=read(fd,data,sizeof data);}while(n<0&&errno==EINTR);
    if(n<0)return -errno;
    if(n!=2||data[1]!='\n'||(data[0]!='0'&&data[0]!='1'))return -EPROTO;
    return (data[0]-'0')==CAMERA_RESET_ON_LEVEL?0:1;
}
