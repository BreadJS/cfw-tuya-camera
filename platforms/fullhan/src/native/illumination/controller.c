#define _GNU_SOURCE
#include "controller.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static uint64_t now_ms(void)
{struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int cut_status(struct illumination *s,struct ircut_status *status){if(CAMERA_HAS_IRCUT)return ircut_status(&s->cut,status);memset(status,0,sizeof *status);status->commanded=IRCUT_DAY;status->idle_ack=1;return 0;}
static int led_status(struct illumination *s,struct ir_led_status *status){if(CAMERA_HAS_IR_LED)return ir_led_get(&s->led,status);memset(status,0,sizeof *status);status->commanded=0;return 0;}
static int set_led(struct illumination *s,unsigned on){if(CAMERA_HAS_IR_LED)return ir_led_set(&s->led,on);return on?-ENOTSUP:0;}
int illumination_init(struct illumination *s,const struct ircut_io *cut,
    void *owner,int(*check)(void *),const struct ir_led_io *led,
    const struct illumination_color_io *color)
{
    if(!s||!cut||!led||!color||!color->read||!color->write||!color->delay_us)return -EINVAL;
    memset(s,0,sizeof *s);s->color=*color;
    int rc=pthread_mutex_init(&s->mutex,NULL);if(rc)return -rc;
    rc=ircut_init(&s->cut,cut,CAMERA_IRCUT_FLIP);if(rc)return rc;
    return ir_led_init(&s->led,owner,check,led);
}
int illumination_prepare(struct illumination *s)
{
    if(!s)return -EINVAL;
    int rc=CAMERA_HAS_IR_LED?ir_led_prepare_off(&s->led):0;
    if(!rc&&CAMERA_HAS_IRCUT)rc=ircut_prepare_idle(&s->cut);
    uint32_t value=0;
    if(!rc)rc=s->color.read(s->color.opaque,0x5d0,&value);
    if(!rc){s->day_saturation=value&0x3ffU;s->prepared=1;}
    s->last_error=rc;return rc;
}
/* Exact stock grayscale path: c4740 sets manual saturation0; fd730 publishes
 * ctx2d9, and112a44 computes sat<<2 then112ba8..112bc0 RMW5d0[9:0]. Day
 * restores just the initially captured daylight field, preserving otherbits. */
static int grayscale(struct illumination *s,unsigned night)
{
    uint32_t value,readback;int rc=s->color.read(s->color.opaque,0x5d0,&value);
    if(rc)return rc;
    value=(value&~0x3ffU)|(night?0U:s->day_saturation);
    rc=s->color.write(s->color.opaque,0x5d0,value);if(rc)return rc;
    rc=s->color.read(s->color.opaque,0x5d0,&readback);if(rc)return rc;
    if(readback!=value)return -EPROTO;
    s->night=night;s->monochrome_known=1;return 0;
}
int illumination_apply(void *opaque,unsigned night,unsigned on)
{
    struct illumination *s=opaque;if(!s||night>1||on>1)return -EINVAL;
    if((!CAMERA_HAS_IRCUT&&night)||(!CAMERA_HAS_IR_LED&&on))return -ENOTSUP;
    int lock=pthread_mutex_trylock(&s->mutex);if(lock)return lock==EBUSY?-EBUSY:-lock;
    struct ircut_status cut;int rc=cut_status(s,&cut);
    if(!rc&&(!s->prepared||cut.faulted))rc=-EPIPE;
    unsigned changed=!rc&&cut.commanded!=(night?IRCUT_NIGHT:IRCUT_DAY);
    uint64_t now=now_ms();
    if(!rc&&changed&&s->last_pulse_ms&&now-s->last_pulse_ms<3000U)rc=-EAGAIN;
    if(rc){pthread_mutex_unlock(&s->mutex);return rc;}
    s->busy=1;
    if(changed) {
        rc=set_led(s,0);
        if(!rc&&night)rc=grayscale(s,1);
        if(!rc&&night)rc=s->color.delay_us(s->color.opaque,50000U);
        if(!rc)rc=ircut_set(&s->cut,night?IRCUT_NIGHT:IRCUT_DAY);
        s->last_pulse_ms=now_ms();
        if(!rc)rc=s->color.delay_us(s->color.opaque,night?500000U:50000U);
    }
    if(!rc&&(!s->monochrome_known||s->night!=night))rc=grayscale(s,night);
    if(!rc)rc=set_led(s,on);
    if(rc) {
        /* Preserve primary error; never claim an optical/position rollback. */
        (void)set_led(s,0);
    }
    s->last_error=rc;s->busy=0;pthread_mutex_unlock(&s->mutex);return rc;
}
int illumination_status_json(void *opaque,char *out,size_t n)
{
    struct illumination *s=opaque;if(!s||!out||!n)return -EINVAL;
    int lock=pthread_mutex_trylock(&s->mutex);
    if(lock==EBUSY){snprintf(out,n,"{\"available\":true,\"filter\":\"unknown\",\"ir_led\":false,\"busy\":true}");return 0;}
    if(lock)return -lock;
    struct ircut_status c={.commanded=IRCUT_UNKNOWN};struct ir_led_status l={.commanded=-1};
    int rc=cut_status(s,&c);if(!rc)rc=led_status(s,&l);
    uint64_t now=now_ms(),remaining=s->last_pulse_ms&&now-s->last_pulse_ms<3000U?3000U-(now-s->last_pulse_ms):0;
    if(!rc) {
        const char *filter=c.commanded==IRCUT_DAY?"day":c.commanded==IRCUT_NIGHT?"night":"unknown";
        int bytes=snprintf(out,n,"{\"available\":%s,\"filter\":\"%s\",\"requested_filter\":\"%s\",\"ir_led\":%s,\"ir_led_known\":%s,\"monochrome\":%s,\"busy\":false,\"cooldown_ms\":%llu,\"last_error\":%d,\"cleanup_error\":%d,\"coil_idle\":%s,\"pulses\":%llu,\"position_feedback\":false}",s->prepared&&!c.faulted&&!l.faulted?"true":"false",filter,filter,l.commanded==1?"true":"false",l.commanded>=0?"true":"false",s->night&&s->monochrome_known?"true":"false",(unsigned long long)remaining,s->last_error,c.cleanup_error,c.idle_ack?"true":"false",(unsigned long long)c.pulses);
        if(bytes<0||(size_t)bytes>=n)rc=-ENOSPC;
    }
    pthread_mutex_unlock(&s->mutex);return rc;
}

int illumination_picture_begin(struct illumination *s,unsigned *night)
{
    if(!s||!night)return -EINVAL;
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return rc==EBUSY?0:-rc;
    struct ircut_status cut;rc=cut_status(s,&cut);
    if(rc||!s->prepared||s->busy||cut.faulted||cut.commanded==IRCUT_UNKNOWN){pthread_mutex_unlock(&s->mutex);return rc?rc:0;}
    *night=s->night;return 1;
}
int illumination_color_begin(struct illumination *s)
{
    if(!s)return -EINVAL;
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return rc==EBUSY?0:-rc;
    struct ircut_status cut;struct ir_led_status led;rc=cut_status(s,&cut);
    if(!rc)rc=led_status(s,&led);
    if(rc||!s->prepared||s->busy||s->night||cut.faulted||cut.commanded!=IRCUT_DAY||led.commanded!=0){pthread_mutex_unlock(&s->mutex);return rc?rc:0;}
    /* Same lock as physical switching: no colour update spans a transition. */
    return 1;
}
void illumination_color_end(struct illumination *s){if(s)pthread_mutex_unlock(&s->mutex);}
