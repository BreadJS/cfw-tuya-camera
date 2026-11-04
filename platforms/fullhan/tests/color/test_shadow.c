#include "../../src/native/isp/isp_shadow.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned seed=1112;
static unsigned word(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void){
 struct fh_shadow_config cfg=FH_SHADOW_DAY_CONFIG;
 struct fh_shadow_state state={0};struct fh_shadow_plan out;
 struct fh_shadow_input in={.generation=1,.width=1920,.height=1080,.histogram={1000000,1000000,50000},.min_log_q8=1000,.max_log_q8=4000};
 assert(fh_shadow_make_plan(&cfg,&state,&in,&out)==0);
 assert((out.reg240&65535)>=0x1100&&(out.reg244&65535)>=0xa100);
 struct fh_shadow_state before=state;
 assert(fh_shadow_make_plan(&cfg,&state,&in,&out)==-ESTALE&&!memcmp(&state,&before,sizeof state));
 in.generation++;in.width=0;
 assert(fh_shadow_make_plan(&cfg,&state,&in,&out)==-EINVAL&&!memcmp(&state,&before,sizeof state));
 in.width=1920;in.histogram[2]=65536;
 assert(fh_shadow_make_plan(&cfg,&state,&in,&out)==-EINVAL&&!memcmp(&state,&before,sizeof state));
 in.histogram[2]=50000;
 for(unsigned n=0;n<10000;n++){
  in.generation++;in.min_log_q8=word()%4000;in.max_log_q8=word()%4096;in.histogram[0]=word()%2000000000;in.histogram[1]=word();in.histogram[2]=word()%65536;
  in.reg23c=word();in.reg240=word();in.reg244=word();in.reg248=word();in.reg24c=word();in.reg250=word();
  assert(fh_shadow_make_plan(&cfg,&state,&in,&out)==0);
  assert((out.reg248&4095)>=512&&((out.reg248>>16)&4095)>=768&&((out.reg248>>16)&4095)<=3584);
  assert((out.reg23c&0xfffc00ff)==(in.reg23c&0xfffc00ff));
 }
 struct fh_shadow_tile tiles[255]={0};uint32_t lo,hi;
 assert(fh_shadow_extrema(tiles,255,&lo,&hi)==0&&lo==0&&hi==0);
 for(unsigned i=0;i<255;i++){tiles[i]=(struct fh_shadow_tile){{40950,40950,40950,40950},10};}
 assert(fh_shadow_extrema(tiles,255,&lo,&hi)==0&&lo<hi);
 assert(fh_shadow_extrema(tiles,1024,&lo,&hi)==-EINVAL);
 puts("Adaptive shadow:10000 bounded plans, freshness/geometry guards, masks/extrema PASS");
}
