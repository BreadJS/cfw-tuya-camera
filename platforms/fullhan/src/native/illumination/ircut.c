#define _GNU_SOURCE
#include "ircut.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int error(int rc){return rc>0?-EIO:rc;}
static int idle(struct ircut *c)
{
    int a=error(c->io.write_pin(c->io.opaque,CAMERA_IRCUT_PIN_A,0));
    int b=error(c->io.write_pin(c->io.opaque,CAMERA_IRCUT_PIN_B,0));
    c->status.cleanup_error=a?a:b;c->status.idle_ack=!(a||b);
    return c->status.cleanup_error;
}
static int prepared(struct ircut *c)
{
    int rc=error(c->io.check_owner(c->io.opaque));if(rc)return rc;
    int a=error(c->io.prepare_pin(c->io.opaque,CAMERA_IRCUT_PIN_A));
    int b=error(c->io.prepare_pin(c->io.opaque,CAMERA_IRCUT_PIN_B));
    rc=idle(c);if(a||b)rc=a?a:b;
    c->status.prepared=!(a||b);c->status.commanded=IRCUT_UNKNOWN;
    c->status.faulted=rc!=0;c->status.last_error=rc;return rc;
}
int ircut_init(struct ircut*c,const struct ircut_io*io,unsigned flip)
{
    if(!c||!io||!io->check_owner||!io->prepare_pin||!io->write_pin||!io->sleep_us||flip>1)return -EINVAL;
    memset(c,0,sizeof *c);c->io=*io;c->flip=flip;c->status.commanded=IRCUT_UNKNOWN;
    int rc=pthread_mutex_init(&c->mutex,NULL);return rc?-rc:0;
}
int ircut_prepare_idle(struct ircut*c)
{
    if(!c)return -EINVAL;
    int rc=pthread_mutex_trylock(&c->mutex);if(rc)return -rc;
    int previous;int cancel=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous);
    if(cancel){pthread_mutex_unlock(&c->mutex);return -cancel;}
    rc=prepared(c);if(rc){c->status.last_error=rc;c->status.faulted=1;}
    pthread_mutex_unlock(&c->mutex);pthread_setcancelstate(previous,NULL);return rc;
}
int ircut_set(struct ircut*c,enum ircut_mode mode)
{
    if(!c||(mode!=IRCUT_DAY&&mode!=IRCUT_NIGHT))return -EINVAL;
    int rc=pthread_mutex_trylock(&c->mutex);if(rc)return -rc;
    int previous;int cancel=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous);
    if(cancel){pthread_mutex_unlock(&c->mutex);return -cancel;}
    rc=error(c->io.check_owner(c->io.opaque));if(rc)goto done;
    if(c->status.faulted){rc=-EUCLEAN;goto done;}
    if(!c->status.prepared){rc=prepared(c);if(rc)goto done;}
    if(c->status.commanded==mode&&c->status.idle_ack){rc=0;goto done;}
    /* Raw2f0b0 r3: forflip1 day=>1/night=>0; flip0 reverses. */
    unsigned fourth=(mode==IRCUT_DAY)?c->flip:!c->flip;
    c->status.idle_ack=0;c->status.commanded=IRCUT_UNKNOWN;
    rc=error(c->io.write_pin(c->io.opaque,CAMERA_IRCUT_PIN_A,fourth?0:1));
    if(!rc)rc=error(c->io.write_pin(c->io.opaque,CAMERA_IRCUT_PIN_B,fourth?1:0));
    if(!rc){++c->status.pulses;rc=error(c->io.sleep_us(c->io.opaque,IRCUT_PULSE_US));}
    int cleanup=idle(c);if(!rc)rc=cleanup;
    if(!rc)c->status.commanded=mode;else c->status.faulted=1;
done:
    c->status.last_error=rc;pthread_mutex_unlock(&c->mutex);pthread_setcancelstate(previous,NULL);return rc;
}
int ircut_status(struct ircut*c,struct ircut_status*out)
{
    if(!c||!out)return -EINVAL;
    int rc=pthread_mutex_trylock(&c->mutex);if(rc)return -rc;
    *out=c->status;pthread_mutex_unlock(&c->mutex);return 0;
}
static int linux_check(void*p)
{struct ircut_linux*l=p;return !l||!l->check?-EPERM:error(l->check(l->owner));}
static int pin_index(unsigned pin){return pin==CAMERA_IRCUT_PIN_A?0:pin==CAMERA_IRCUT_PIN_B?1:-1;}
static int text_write(const char*path,const char*text,size_t bytes)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    ssize_t n=write(fd,text,bytes);int rc=n<0?-errno:(size_t)n!=bytes?-EIO:0;
    if(close(fd)&&!rc)rc=-errno;
    return rc;
}
static int linux_prepare(void*p,unsigned pin)
{
    struct ircut_linux*l=p;int index=pin_index(pin);if(index<0)return -EINVAL;
    int rc=linux_check(l);if(rc)return rc;
    char path[96],number[4];snprintf(number,sizeof number,"%u",pin);
    rc=text_write("/sys/class/gpio/export",number,strlen(number));
    /* Existing exports are usable only under the same mandatory whole-owner
     * attestation; EBUSY is not itself ownership proof. */
    if(rc&&rc!=-EBUSY)return rc;
    snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/direction",pin);
    rc=text_write(path,"out",3);if(rc)return rc;
    if(l->value_fd[index]<0) {
        snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/value",pin);
        int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
        l->value_fd[index]=fd;
    }
    return 0;
}
static int linux_value(void*p,unsigned pin,unsigned value)
{
    struct ircut_linux*l=p;int index=pin_index(pin);if(index<0||value>1)return -EINVAL;
    int rc=linux_check(l);if(rc)return rc;
    int fd=l->value_fd[index];if(fd<0)return -ENODEV;
    if(lseek(fd,0,SEEK_SET)<0)return -errno;
    char text=value?'1':'0';ssize_t n=write(fd,&text,1);return n<0?-errno:n!=1?-EIO:0;
}
static int linux_sleep(void*p,unsigned us)
{
    (void)p;if(us!=IRCUT_PULSE_US)return -EINVAL;
    struct timespec delay={.tv_sec=0,.tv_nsec=(long)us*1000};
    /* EINTR goes to immediate idle cleanup, never leaves coil intentionally
     * energized while retrying an interrupted delay. */
    return nanosleep(&delay,NULL)?-errno:0;
}
int ircut_linux_init(struct ircut_linux*l,void*owner,int(*check)(void*))
{if(!l||!check)return -EINVAL;*l=(struct ircut_linux){owner,check,{-1,-1}};return 0;}
struct ircut_io ircut_linux_io(struct ircut_linux*l)
{return (struct ircut_io){l,linux_check,linux_prepare,linux_value,linux_sleep};}
