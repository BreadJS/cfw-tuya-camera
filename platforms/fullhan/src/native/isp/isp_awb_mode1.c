#include "isp_awb_mode1.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
static uint32_t bounded(uint64_t x){return x<256?256:x>2048?2048:(uint32_t)x;}
static uint32_t step(uint32_t x,uint32_t target){unsigned d=x>target?x-target:target-x;if(d<=2)return x;unsigned n=(d+15)/16;if(n>32)n=32;return x<target?x+n:x-n;}
static struct fh_awb_gains inverse(uint64_t r,uint64_t g,uint64_t b){uint64_t m=r<g?r:g;if(b<m)m=b;return(struct fh_awb_gains){(uint32_t)(m*512/r),(uint32_t)(m*512/g),(uint32_t)(m*512/b)};}
int fh_awb_mode1_target(const struct fh_awb_mode1_config *cfg,const struct fh_awb_sample *s,struct fh_awb_mode1_result *out){
 if(!cfg||!s||!out||!cfg->minimum_mean||cfg->saturation_mean<=cfg->minimum_mean||cfg->minimum_cells<3||cfg->minimum_cells>9||cfg->spread_low>cfg->spread_middle||cfg->spread_middle>cfg->spread_high)return -EINVAL;
 uint64_t r=0,g=0,b=0,total_count=0;uint32_t ratios[9]={0},lo=0x10000,hi=0;unsigned indices[9],valid=0;
 for(unsigned i=0;i<9;i++){
  indices[i]=i;const struct fh_awb_cell *c=&s->cell[i];total_count+=c->count;
  if(c->count<11||!c->red||!c->green||!c->blue)continue;
  uint64_t means[3]={((uint64_t)c->red+c->count/2)/c->count,((uint64_t)c->green+c->count/2)/c->count,((uint64_t)c->blue+c->count/2)/c->count};
  if(means[0]<cfg->minimum_mean||means[1]<cfg->minimum_mean||means[2]<cfg->minimum_mean||means[0]>=cfg->saturation_mean||means[1]>=cfg->saturation_mean||means[2]>=cfg->saturation_mean)continue;
  uint64_t rb=(uint64_t)c->red*4096/c->blue,bg=(uint64_t)c->blue*4096/c->green,rg=(uint64_t)c->red*4096/c->green;
  if(rb>=UINT32_MAX||bg>=UINT32_MAX||rg>=UINT32_MAX)continue;
  ratios[i]=(uint32_t)rb;if(rb<lo)lo=(uint32_t)rb;if(rb>hi)hi=(uint32_t)rb;
  r+=c->red;g+=c->green;b+=c->blue;valid++;
 }
 struct fh_awb_mode1_result result={0};result.accepted_cells=valid;result.selected_cell=UINT_MAX;
 if(valid<cfg->minimum_cells||total_count<90){*out=result;return 0;}
 /*0x10aa8c is an exchange sort, not a stable insertion sort: ties can move
  * when a later smaller element swaps through them. Keep identical order. */
 for(unsigned i=0;i<8;i++)for(unsigned j=i+1;j<9;j++)if(ratios[j]<ratios[i]){uint32_t q=ratios[i];ratios[i]=ratios[j];ratios[j]=q;unsigned k=indices[i];indices[i]=indices[j];indices[j]=k;}
 unsigned selected=indices[(9+(9-valid))/2];result.selected_cell=selected;
 uint32_t spread=hi-lo,low=(uint32_t)cfg->spread_middle*128,delta=((uint32_t)cfg->spread_high-cfg->spread_middle)*128;
 unsigned weight;
 if(spread>=low+delta*3/4&&lo<=0xce3)weight=8;
 else if(spread>=low+delta/2&&lo<=0xe65)weight=7;
 else if(spread>=low+delta/4)weight=6;
 else if(spread>=low)weight=4;
 else {uint32_t first=(uint32_t)cfg->spread_low*128,range=low-first;if(spread>=first+range*2/3)weight=3;else if(spread>=first+range/3)weight=2;else weight=spread>=first?1:0;}
 const struct fh_awb_cell *c=&s->cell[selected];struct fh_awb_gains cell=inverse(c->red,c->green,c->blue),all=inverse(r,g,b);
 result.pre_luma=(struct fh_awb_gains){(weight*cell.red+(8-weight)*all.red)/8,(weight*cell.green+(8-weight)*all.green)/8,(weight*cell.blue+(8-weight)*all.blue)/8};
 if(!result.pre_luma.green||!result.pre_luma.red||!result.pre_luma.blue){*out=result;return 0;}
 result.gains=(struct fh_awb_gains){bounded((uint64_t)result.pre_luma.red*512/result.pre_luma.green),512,bounded((uint64_t)result.pre_luma.blue*512/result.pre_luma.green)};result.selected_weight=weight;*out=result;return 1;
}
int fh_awb_mode1_plan(const struct fh_awb_mode1_config *cfg,struct fh_awb_state *state,const struct fh_awb_sample *sample,struct fh_awb_mode1_result *out){
 if(!state||!sample||!out||!state->initialized||!sample->generation||state->gains.green!=512||state->gains.red<256||state->gains.red>2048||state->gains.blue<256||state->gains.blue>2048)return -EINVAL;
 if(sample->generation<=state->generation)return -ESTALE;
 struct fh_awb_mode1_result result;int rc=fh_awb_mode1_target(cfg,sample,&result);if(rc<0)return rc;
 state->generation=sample->generation;
 if(rc){result.gains.red=step(state->gains.red,result.gains.red);result.gains.blue=step(state->gains.blue,result.gains.blue);result.changed=result.gains.red!=state->gains.red||result.gains.blue!=state->gains.blue;state->gains=result.gains;}
 else result.gains=state->gains;
 *out=result;return rc;
}
