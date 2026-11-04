#define _GNU_SOURCE
#include "status_led.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int checked(int rc){return rc>0?-EIO:rc;}
static int readback(struct status_led *s)
{
    unsigned a=0,b=0;s->state.levels_known=0;
    int x=checked(s->io.read_pin(s->io.opaque,CAMERA_STATUS_LED_A_PIN,&a));
    int y=checked(s->io.read_pin(s->io.opaque,CAMERA_STATUS_LED_B_PIN,&b));
    if(x||y)return x?x:y;
    if(a>1||b>1)return -EPROTO;
    s->state.high_mask=a|(b<<1);s->state.levels_known=1;return 0;
}
static int alloff(struct status_led *s)
{
    int a=checked(s->io.write_pin(s->io.opaque,CAMERA_STATUS_LED_A_PIN,!CAMERA_STATUS_LED_A_ON_LEVEL));
    int b=checked(s->io.write_pin(s->io.opaque,CAMERA_STATUS_LED_B_PIN,!CAMERA_STATUS_LED_B_ON_LEVEL));
    s->state.cleanup_error=a?a:b;return s->state.cleanup_error;
}
int status_led_init(struct status_led*s,const struct status_led_io*io)
{
    if(!s||!io||!io->check_owner||!io->prepare_pin||!io->write_pin||!io->read_pin)return -EINVAL;
    memset(s,0,sizeof *s);s->io=*io;int rc=pthread_mutex_init(&s->mutex,NULL);return rc?-rc:0;
}
int status_led_set(struct status_led*s,unsigned mask)
{
    if(!s||mask>3)return -EINVAL;
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return -rc;
    int previous;rc=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous);
    if(rc){pthread_mutex_unlock(&s->mutex);return -rc;}
    rc=checked(s->io.check_owner(s->io.opaque));if(rc)goto done;
    s->state.commanded_known=0;s->state.levels_known=0;s->state.cleanup_error=0;
    if(!s->state.prepared) {
        int a=checked(s->io.prepare_pin(s->io.opaque,CAMERA_STATUS_LED_A_PIN));
        int b=checked(s->io.prepare_pin(s->io.opaque,CAMERA_STATUS_LED_B_PIN));
        if(a||b){rc=a?a:b;alloff(s);readback(s);goto done;}
        s->state.prepared=1;
    }
    rc=checked(s->io.write_pin(s->io.opaque,CAMERA_STATUS_LED_A_PIN,(mask&STATUS_LED_CHANNEL_A)?CAMERA_STATUS_LED_A_ON_LEVEL:!CAMERA_STATUS_LED_A_ON_LEVEL));
    if(!rc)rc=checked(s->io.write_pin(s->io.opaque,CAMERA_STATUS_LED_B_PIN,(mask&STATUS_LED_CHANNEL_B)?CAMERA_STATUS_LED_B_ON_LEVEL:!CAMERA_STATUS_LED_B_ON_LEVEL));
    if(rc){alloff(s);readback(s);goto done;}
    rc=readback(s);if(rc)goto done;
    if(s->state.high_mask!=(unsigned)(((mask&1)?CAMERA_STATUS_LED_A_ON_LEVEL:!CAMERA_STATUS_LED_A_ON_LEVEL)|(((mask&2)?CAMERA_STATUS_LED_B_ON_LEVEL:!CAMERA_STATUS_LED_B_ON_LEVEL)<<1))){rc=-EPROTO;goto done;}
    s->state.on_mask=mask;s->state.commanded_known=1;
done:
    s->state.last_error=rc;pthread_mutex_unlock(&s->mutex);
    pthread_setcancelstate(previous,NULL);return rc;
}
int status_led_get(struct status_led*s,struct status_led_state*out)
{
    if(!s||!out)return -EINVAL;
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return -rc;
    rc=checked(s->io.check_owner(s->io.opaque));
    if(!rc&&s->state.prepared)rc=readback(s);
    if(rc){s->state.levels_known=0;s->state.last_error=rc;}
    *out=s->state;pthread_mutex_unlock(&s->mutex);return rc;
}
static int indexof(unsigned pin){return pin==CAMERA_STATUS_LED_A_PIN?0:pin==CAMERA_STATUS_LED_B_PIN?1:-1;}
static int owner(void*p)
{struct status_led_linux*l=p;return !l||!l->check?-EPERM:checked(l->check(l->owner));}
static int put_text(const char*path,const char*text)
{
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    size_t size=strlen(text);ssize_t n=write(fd,text,size);int rc=n<0?-errno:(size_t)n!=size?-EIO:0;
    if(close(fd)&&!rc)rc=-errno;
    return rc;
}
static int linux_prepare(void*p,unsigned pin)
{
    struct status_led_linux*l=p;int index=indexof(pin);if(index<0)return -EINVAL;
    int rc=owner(l);if(rc)return rc;
    char text[64],path[96];
    snprintf(text,sizeof text,"dev,GPIO%u,0,0\n",pin);
    rc=put_text("/proc/driver/pinctrl",text);if(rc)return rc;
    snprintf(text,sizeof text,"mux,GPIO%u,GPIO%u,0\n",pin,pin);
    rc=put_text("/proc/driver/pinctrl",text);if(rc)return rc;
    snprintf(text,sizeof text,"%u",pin);rc=put_text("/sys/class/gpio/export",text);
    if(rc&&rc!=-EBUSY)return rc;
    /* Never silently interpret an inverted sysfs descriptor as physicallevel.
     * Stock leaves active_low at its default0. No attribute change here. */
    snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/active_low",pin);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    char value[4];ssize_t n=read(fd,value,sizeof value);rc=n<0?-errno:n<1||value[0]!='0'?-ENOTSUP:0;
    if(close(fd)&&!rc)rc=-errno;
    if(rc)return rc;
    snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/direction",pin);
    /* Matching kernel direction_store(c01cd4f8) supports high→rawoutput1.
     * This avoids stock out→initial0 flashing an active-low channel. */
    rc=put_text(path,((index?CAMERA_STATUS_LED_B_ON_LEVEL:CAMERA_STATUS_LED_A_ON_LEVEL)?"low":"high"));if(rc)return rc;
    if(l->value_fd[index]<0) {
        snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/value",pin);
        fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;l->value_fd[index]=fd;
    }
    return 0;
}
static int linux_write(void*p,unsigned pin,unsigned value)
{
    struct status_led_linux*l=p;int index=indexof(pin);if(index<0||value>1)return -EINVAL;
    int rc=owner(l);if(rc)return rc;
    int fd=l->value_fd[index];if(fd<0)return -ENODEV;
    if(lseek(fd,0,SEEK_SET)<0)return -errno;
    char c=value?'1':'0';ssize_t n=write(fd,&c,1);return n<0?-errno:n!=1?-EIO:0;
}
static int linux_read(void*p,unsigned pin,unsigned*out)
{
    struct status_led_linux*l=p;int index=indexof(pin);if(index<0||!out)return -EINVAL;
    int rc=owner(l);if(rc)return rc;
    int fd=l->value_fd[index];if(fd<0)return -ENODEV;
    if(lseek(fd,0,SEEK_SET)<0)return -errno;
    char text[4];ssize_t n=read(fd,text,sizeof text);
    if(n<0)return -errno;
    if(n<1||text[0]<'0'||text[0]>'1')return -EPROTO;
    *out=(unsigned)(text[0]-'0');return 0;
}
int status_led_linux_init(struct status_led_linux*l,void*p,int(*check)(void*))
{if(!l||!check)return -EINVAL;*l=(struct status_led_linux){p,check,{-1,-1}};return 0;}
struct status_led_io status_led_linux_io(struct status_led_linux*l)
{return (struct status_led_io){l,owner,linux_prepare,linux_write,linux_read};}
