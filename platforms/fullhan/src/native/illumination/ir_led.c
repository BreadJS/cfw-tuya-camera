#define _GNU_SOURCE
#include "ir_led.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#define SYSFS_MAGIC 0x62656572

int ir_led_init(struct ir_led *s,void *owner,int (*check)(void *),const struct ir_led_io *io)
{
    int rc;
    if(!s||!owner||!check||!io||!io->prepare||!io->write)return -EINVAL;
    memset(s,0,sizeof(*s));s->owner=owner;s->check_owner=check;s->io=*io;
    s->status.commanded=-1;rc=pthread_mutex_init(&s->mutex,0);return rc?-rc:0;
}
static int finish(struct ir_led *s,int rc)
{
    s->status.last_error=rc;
    if(rc){s->status.faulted=1;s->status.commanded=-1;}
    pthread_mutex_unlock(&s->mutex);return rc;
}
int ir_led_prepare_off(struct ir_led *s)
{
    int rc;if(!s)return -EINVAL;
    rc=pthread_mutex_lock(&s->mutex);if(rc)return -rc;
    rc=s->check_owner(s->owner);if(rc)return finish(s,rc);
    rc=s->io.prepare(s->io.opaque);if(rc)return finish(s,rc);
    rc=s->check_owner(s->owner);if(rc)return finish(s,rc);
    rc=s->io.write(s->io.opaque,0);if(rc)return finish(s,rc);
    s->status.prepared=1;s->status.commanded=0;s->status.faulted=0;
    return finish(s,0);
}
int ir_led_set(struct ir_led *s,unsigned on)
{
    int rc;if(!s||on>1)return -EINVAL;
    rc=pthread_mutex_lock(&s->mutex);if(rc)return -rc;
    rc=s->check_owner(s->owner);if(rc)return finish(s,rc);
    if(!s->status.prepared||s->status.faulted)return finish(s,-EPERM);
    rc=s->io.write(s->io.opaque,on);if(rc)return finish(s,rc);
    s->status.commanded=(int)on;return finish(s,0);
}
int ir_led_get(struct ir_led *s,struct ir_led_status *out)
{
    int rc;if(!s||!out)return -EINVAL;
    rc=pthread_mutex_trylock(&s->mutex);if(rc)return -rc;
    *out=s->status;pthread_mutex_unlock(&s->mutex);return 0;
}
static int valid_fd(int fd)
{
    struct stat st;struct statfs fs;
    if(fstat(fd,&st)||fstatfs(fd,&fs))return -errno;
    if(!S_ISREG(st.st_mode)||st.st_uid!=0||fs.f_type!=SYSFS_MAGIC)return -EPERM;
    return 0;
}
static int open_fixed(const char *path)
{
    int fd,rc;do{fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);}while(fd<0&&errno==EINTR);
    if(fd<0)return -errno;
    rc=valid_fd(fd);if(rc){(void)close(fd);return rc;}return fd;
}
/* A sysfs attribute is one command: short writes are errors, never split into
 * a second command. EINTR before any progress is safely retried. */
static int write_command(int fd,const char *value,size_t n,int seek)
{
    ssize_t got;
    if(seek){off_t off;do{off=lseek(fd,0,SEEK_SET);}while(off<0&&errno==EINTR);
        if(off<0)return -errno;
        if(off!=0)return -EIO;}
    do{got=write(fd,value,n);}while(got<0&&errno==EINTR);
    if(got<0)return -errno;
    return (size_t)got==n?0:-EIO;
}
static int transient(const char *path,const char *value,size_t n)
{
    int fd=open_fixed(path),rc,err;if(fd<0)return fd;
    rc=write_command(fd,value,n,0);err=close(fd);if(!rc&&err)rc=-errno;return rc;
}
static int linux_prepare(void *opaque)
{
    struct ir_led_linux *s=opaque;int rc,fd;
    rc=s->check(s->owner);if(rc)return rc;
    rc=transient("/sys/class/gpio/export",CAMERA_IR_LED_EXPORT,sizeof(CAMERA_IR_LED_EXPORT)-1);
    if(rc&&rc!=-EBUSY)return rc;
    rc=s->check(s->owner);if(rc)return rc;
    rc=transient(CAMERA_IR_LED_DIRECTION,"out",3);if(rc)return rc;
    rc=s->check(s->owner);if(rc)return rc;
    if(s->value_fd>=0)return valid_fd(s->value_fd);
    fd=open_fixed(CAMERA_IR_LED_VALUE);if(fd<0)return fd;
    s->value_fd=fd;return 0;
}
static int linux_write(void *opaque,unsigned on)
{
    struct ir_led_linux *s=opaque;int rc;if(on>1)return -EINVAL;
    rc=s->check(s->owner);if(rc)return rc;
    if(s->value_fd<0)return -EBADF;
    rc=valid_fd(s->value_fd);if(rc)return rc;
    return write_command(s->value_fd,(on?CAMERA_IR_LED_ON_LEVEL:!CAMERA_IR_LED_ON_LEVEL)?"1":"0",1,1);
}
int ir_led_linux_init(struct ir_led_linux *s,void *owner,int (*check)(void *))
{
    if(!s||!owner||!check)return -EINVAL;
    s->owner=owner;s->check=check;s->value_fd=-1;return 0;
}
struct ir_led_io ir_led_linux_io(struct ir_led_linux *s)
{
    struct ir_led_io io={s,linux_prepare,linux_write};return io;
}
