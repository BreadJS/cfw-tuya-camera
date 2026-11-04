#include "isp_awb.h"
#include <errno.h>
#include <string.h>
struct candidate {uint32_t red,blue,brightness,count;};
static uint32_t clamp(uint64_t value,uint32_t low,uint32_t high)
{return value<low?low:value>high?high:(uint32_t)value;}
static uint32_t median(uint32_t *values,unsigned count)
{
    for(unsigned i=1;i<count;i++) {
        uint32_t value=values[i];unsigned at=i;
        while(at&&values[at-1]>value){values[at]=values[at-1];at--;}
        values[at]=value;
    }
    return values[count/2];
}
static unsigned distance(uint32_t a,uint32_t b){return a>b?a-b:b-a;}
static uint32_t smooth(uint32_t current,uint32_t target)
{
    unsigned difference=distance(current,target);
    if(difference<=2)return current;
    unsigned step=(difference+15)/16;if(step>32)step=32;
    return target>current?current+step:current-step;
}
int fh_awb_init(struct fh_awb_state *state,const struct fh_awb_gains *initial)
{
    if(!state||!initial||initial->red<256||initial->red>2048||initial->green<256||initial->green>2048||initial->blue<256||initial->blue>2048)return -EINVAL;
    struct fh_awb_state result={0};result.initialized=1;result.gains.green=512;
    result.gains.red=clamp(((uint64_t)initial->red*512+initial->green/2)/initial->green,256,2048);
    result.gains.blue=clamp(((uint64_t)initial->blue*512+initial->green/2)/initial->green,256,2048);
    *state=result;return 0;
}
static int config_valid(const struct fh_awb_config *c)
{
    return c&&c->minimum_mean&&c->saturation_mean>c->minimum_mean&&
        c->red_ratio_min&&c->red_ratio_min<c->red_ratio_max&&c->red_ratio_max<=8192&&
        c->blue_ratio_min&&c->blue_ratio_min<c->blue_ratio_max&&c->blue_ratio_max<=8192&&
        c->minimum_cells>=3&&c->minimum_cells<=FH_AWB_CELLS;
}
int fh_awb_plan(const struct fh_awb_config *config,struct fh_awb_state *state,
                const struct fh_awb_sample *sample,struct fh_awb_result *out)
{
    if(!config_valid(config)||!state||!state->initialized||!sample||!out||!sample->generation||
       state->gains.red<256||state->gains.red>2048||state->gains.green!=512||
       state->gains.blue<256||state->gains.blue>2048)return -EINVAL;
    if(sample->generation<=state->generation)return -ESTALE;
    struct candidate candidate[FH_AWB_CELLS];unsigned count=0;
    uint32_t reds[FH_AWB_CELLS],blues[FH_AWB_CELLS];
    for(unsigned i=0;i<FH_AWB_CELLS;i++) {
        const struct fh_awb_cell *c=&sample->cell[i];
        if(!c->count||!c->red||!c->green||!c->blue)continue;
        uint32_t r=(uint32_t)(((uint64_t)c->red+c->count/2)/c->count);
        uint32_t g=(uint32_t)(((uint64_t)c->green+c->count/2)/c->count);
        uint32_t b=(uint32_t)(((uint64_t)c->blue+c->count/2)/c->count);
        if(r<config->minimum_mean||g<config->minimum_mean||b<config->minimum_mean||
           r>=config->saturation_mean||g>=config->saturation_mean||b>=config->saturation_mean)continue;
        uint64_t red=((uint64_t)c->red*512+c->green/2)/c->green;
        uint64_t blue=((uint64_t)c->blue*512+c->green/2)/c->green;
        if(red<config->red_ratio_min||red>config->red_ratio_max||blue<config->blue_ratio_min||blue>config->blue_ratio_max)continue;
        candidate[count]=(struct candidate){(uint32_t)red,(uint32_t)blue,(uint32_t)(((uint64_t)r+2*(uint64_t)g+b)/4),c->count};
        reds[count]=(uint32_t)red;blues[count]=(uint32_t)blue;count++;
    }
    struct fh_awb_result result={.gains=state->gains};
    state->generation=sample->generation;
    if(count<config->minimum_cells){*out=result;return 0;}
    uint32_t red_median=median(reds,count),blue_median=median(blues,count);
    /* A single bright/large colored cell must not dictate chromaticity. Find
     * a spatial ratio consensus before assigning count/luminance weights. */
    uint32_t red_tolerance=red_median/4+8,blue_tolerance=blue_median/4+8;
    uint32_t counts[FH_AWB_CELLS],brightness[FH_AWB_CELLS];unsigned accepted=0;
    for(unsigned i=0;i<count;i++) {
        if(distance(candidate[i].red,red_median)>red_tolerance||distance(candidate[i].blue,blue_median)>blue_tolerance)continue;
        candidate[accepted]=candidate[i];counts[accepted]=candidate[i].count;brightness[accepted]=candidate[i].brightness;accepted++;
    }
    if(accepted<config->minimum_cells){*out=result;return 0;}
    uint32_t typical_count=median(counts,accepted),typical_brightness=median(brightness,accepted);
    uint64_t red_sum=0,blue_sum=0,total=0;
    for(unsigned i=0;i<accepted;i++) {
        /* Each component's influence is capped at2x the consensus median.
         * Fixed Q8 weights avoid raw SUM*COUNT accumulation overflow. */
        uint64_t count_weight=clamp(((uint64_t)candidate[i].count*256+typical_count/2)/typical_count,1,512);
        uint64_t light_weight=clamp(((uint64_t)candidate[i].brightness*256+typical_brightness/2)/typical_brightness,1,512);
        uint64_t weight=count_weight*light_weight;
        red_sum+=weight*candidate[i].red;blue_sum+=weight*candidate[i].blue;total+=weight;
    }
    uint32_t red_ratio=(uint32_t)((red_sum+total/2)/total),blue_ratio=(uint32_t)((blue_sum+total/2)/total);
    uint32_t target_red=clamp((UINT64_C(512)*512+red_ratio/2)/red_ratio,256,2048);
    uint32_t target_blue=clamp((UINT64_C(512)*512+blue_ratio/2)/blue_ratio,256,2048);
    result.gains.red=smooth(state->gains.red,target_red);result.gains.blue=smooth(state->gains.blue,target_blue);
    result.accepted_cells=accepted;
    result.changed=result.gains.red!=state->gains.red||result.gains.blue!=state->gains.blue;
    state->gains=result.gains;*out=result;return 1;
}
