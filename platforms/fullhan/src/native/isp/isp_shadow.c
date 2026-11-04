#include "isp_shadow.h"
#include <errno.h>
#include <limits.h>
static uint32_t log_q8(uint32_t x){unsigned n=0;for(uint32_t v=x;v>1;v>>=1)n++;return n*256+((uint64_t)(x-(1u<<n))*256>>n);}
int fh_shadow_extrema(const struct fh_shadow_tile *tiles,unsigned count,uint32_t *low,uint32_t *high){
 if(!tiles||!low||!high||count>1023)return -EINVAL;
 uint32_t lo=4095,hi=0;
 for(unsigned i=0;i<count;i++){
  uint32_t sum=tiles[i].sum[0]+tiles[i].sum[1]+tiles[i].sum[2]+tiles[i].sum[3];
  uint32_t mean=(sum/(tiles[i].count?tiles[i].count:1)>>2)<<4;
  if(mean<lo)lo=mean;
  if(mean>hi)hi=mean;
 }
 *low=log_q8(lo?lo:1);*high=log_q8(hi?hi:1);return 0;
}
static uint32_t clamp(uint32_t v,uint32_t lo,uint32_t hi){return v<lo?lo:v>hi?hi:v;}
int fh_shadow_make_plan(const struct fh_shadow_config *cfg,struct fh_shadow_state *state,const struct fh_shadow_input *in,struct fh_shadow_plan *out){
 if(!cfg||!state||!in||!out||!in->generation||!in->width||!in->height||in->width>2047||in->height>2047||in->min_log_q8>8192||in->max_log_q8>8192||in->histogram[2]>65535)return -EINVAL;
 if(in->generation<=state->generation)return -ESTALE;
 uint32_t area=in->width*in->height;if(area<64)return -ERANGE;
 uint32_t hist0=in->histogram[0]/(area>>6);if(hist0>65535)return -ERANGE;
 if(state->previous_min>8192||state->previous_max>8192||state->smooth_min>8192||state->smooth_max>8192||state->hist0>65535||state->hist2>65535)return -EINVAL;
 struct fh_shadow_state next=*state;
 uint32_t low=(state->smooth_min+((in->min_log_q8+state->previous_min)>>1)*4)/5;
 uint32_t high=(state->smooth_max+((in->max_log_q8+state->previous_max)>>1)*4)/5;
 uint32_t lower=clamp((state->hist0+hist0*4)/5,0x1100,0xffff);
 uint32_t upper=clamp((state->hist2+in->histogram[2]*4)/5,0xa100,0xffff);
 uint32_t target=((uint32_t)cfg->ae_target*cfg->ae_target)>>4;if(!target)target=1;
 uint32_t mean=((uint64_t)state->mean_mass*8/area+log_q8(target<<4)*4)/5;
 uint32_t middle=clamp(mean*cfg->middle_scale>>6,0x300,0xe00);
 uint32_t minimum=low*(cfg->low_scale?cfg->low_scale:1)>>6;
 if(minimum>=middle-0x100)minimum=middle-0x100;
 if(minimum<0x200)minimum=0x200;
 uint32_t maximum=high*cfg->high_scale>>6;if(maximum>0xfff)maximum=0xfff;
 uint32_t knee=in->reg24c&0x3ff;
 uint32_t slope_low=knee*4096/(middle-minimum);
 uint32_t gap=maximum>middle+0x100?maximum-middle:0x100;
 uint32_t slope_high=(0x3ff-knee)*4096/gap;
 uint32_t intensity=((in->histogram[1]/(area>>2))+8)>>4;
 double x=intensity;
 double curve=((x*2.4e-7)*x)*x+(x*5.9e-6)*x-x*0.00073+0.024+(cfg->shadow_bias&15)/10.0;
 uint32_t shadow=0;if(curve>0){double v=curve*1024+0.5;shadow=v>=1023?1023:(uint32_t)v;}
 struct fh_shadow_plan result={
  .reg23c=(in->reg23c&0xfffc00ff)|(shadow<<8),
  .reg240=(in->reg240&0xffff0000)|lower,
  .reg244=(in->reg244&0xffff0000)|upper,
  .reg248=(in->reg248&0xf000f000)|minimum|(middle<<16),
  .reg24c=(in->reg24c&0x3ff)|(slope_low<<10),
  .reg250=(in->reg250&0xffc00000)|slope_high
 };
 next.previous_min=in->min_log_q8;next.previous_max=in->max_log_q8;
 next.smooth_min=low;next.smooth_max=high;next.hist0=lower;next.hist2=upper;
 next.mean_mass=(mean>>3)*area;next.generation=in->generation;
 *state=next;*out=result;return 0;
}
