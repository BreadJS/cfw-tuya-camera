#include "controller.h"
#include <errno.h>
#include <stdio.h>
int indicator_init(struct indicator *s,const struct status_led_io *io,unsigned red,unsigned green)
{
    if(!s||(red!=1&&red!=2)||(green!=1&&green!=2)||red==green)return -EINVAL;
    s->red_mask=red;s->green_mask=green;return status_led_init(&s->hardware,io);
}
int indicator_prepare_off(struct indicator *s)
{return s?status_led_set(&s->hardware,0):-EINVAL;}
int indicator_apply(void *opaque,unsigned mode)
{
    struct indicator *s=opaque;if(!s||mode>3)return -EINVAL;
    return status_led_set(&s->hardware,mode==0?0:mode==1?s->red_mask:mode==2?s->green_mask:3);
}
int indicator_status_json(void *opaque,char *out,size_t size)
{
    struct indicator *s=opaque;if(!s||!out||!size)return -EINVAL;
    struct status_led_state state;int rc=status_led_get(&s->hardware,&state);if(rc)return rc;
    const char *mode="unknown";
    if(state.levels_known) {
        unsigned on=state.high_mask^((CAMERA_STATUS_LED_A_ON_LEVEL?0U:1U)|(CAMERA_STATUS_LED_B_ON_LEVEL?0U:2U));
        mode=on==0?"off":on==s->red_mask?"red":on==s->green_mask?"green":on==3?"both":"unknown";
    }
    int n=snprintf(out,size,"{\"available\":%s,\"mode\":\"%s\",\"last_error\":%d,\"cleanup_error\":%d,\"gpio_readback\":%s,\"optical_feedback\":false}",state.prepared&&state.levels_known?"true":"false",mode,state.last_error,state.cleanup_error,state.levels_known?"true":"false");
    return n<0||(size_t)n>=size?-ENOSPC:0;
}
