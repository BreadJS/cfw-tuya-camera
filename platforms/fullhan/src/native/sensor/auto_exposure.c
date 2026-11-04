#include "auto_exposure.h"
#include <errno.h>

static uint64_t clamp(uint64_t value,uint64_t low,uint64_t high)
{ return value<low?low:value>high?high:value; }
static uint32_t quantize(const struct fh_ae_config *c,uint32_t lines)
{
    if(!c->shutter_quantum_num)return lines;
    uint64_t cycles=(uint64_t)lines*c->shutter_quantum_den/c->shutter_quantum_num;
    if(!cycles)return lines;
    uint32_t result=(uint32_t)((cycles*c->shutter_quantum_num+c->shutter_quantum_den/2)/c->shutter_quantum_den);
    if(result>c->max_lines && cycles>1)
        result=(uint32_t)(((cycles-1)*c->shutter_quantum_num+c->shutter_quantum_den/2)/c->shutter_quantum_den);
    return result>=c->min_lines && result<=c->max_lines?result:lines;
}
int fh_ae_plan(const struct fh_ae_config *c,const struct fh_ae_setting *actual,
               uint32_t brightness,struct fh_ae_setting *out)
{
    if(!c || !actual || !out || !c->target_q16 || c->target_q16>=65535 ||
       brightness>65535 || !c->min_lines || c->max_lines>2245 ||
       c->min_lines>c->max_lines || c->min_gain_q6<64 || c->max_gain_q6>1024 ||
       c->min_gain_q6>c->max_gain_q6 || c->deadband_percent>25 ||
       ((!c->shutter_quantum_num)!=(!c->shutter_quantum_den)) ||
       (c->shutter_quantum_num && (c->shutter_quantum_den>1000 ||
        c->shutter_quantum_num<c->shutter_quantum_den ||
        c->shutter_quantum_num>(uint64_t)c->max_lines*c->shutter_quantum_den)) ||
       !actual->lines || actual->lines>65535 ||
       actual->gain_q6<64 || actual->gain_q6>1024)return -EINVAL;
    uint64_t current=(uint64_t)actual->lines*actual->gain_q6;
    uint64_t minimum=(uint64_t)c->min_lines*c->min_gain_q6;
    uint64_t maximum=(uint64_t)c->max_lines*c->max_gain_q6;
    unsigned inside=actual->lines>=c->min_lines && actual->lines<=c->max_lines &&
        actual->gain_q6>=c->min_gain_q6 && actual->gain_q6<=c->max_gain_q6 &&
        quantize(c,actual->lines)==actual->lines;
    uint32_t difference=brightness>c->target_q16?brightness-c->target_q16:c->target_q16-brightness;
    if(inside && (uint64_t)difference*100<=(uint64_t)c->target_q16*c->deadband_percent) {
        *out=*actual;return 0;
    }
    uint64_t desired=(current*c->target_q16+(brightness?brightness:1)/2)/(brightness?brightness:1);
    if(inside)desired=clamp(desired,(current*2+2)/3,current*3/2);
    desired=clamp(desired,minimum,maximum);
    struct fh_ae_setting next;
    next.lines=(uint32_t)clamp((desired+c->min_gain_q6/2)/c->min_gain_q6,c->min_lines,c->max_lines);
    next.lines=quantize(c,next.lines);
    next.gain_q6=(uint32_t)clamp((desired+next.lines/2)/next.lines,c->min_gain_q6,c->max_gain_q6);
    /* If quantization and maximum gain leave insufficient integration, the
     * hard shutter limit wins over the optional flicker preference. */
    if(!c->shutter_quantum_num && (uint64_t)next.lines*next.gain_q6<desired && next.gain_q6==c->max_gain_q6)
        next.lines=(uint32_t)clamp((desired+next.gain_q6/2)/next.gain_q6,c->min_lines,c->max_lines);
    *out=next;
    return next.lines!=actual->lines || next.gain_q6!=actual->gain_q6;
}
