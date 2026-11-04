#include "../../src/native/isp/isp_tone_profile.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void){
 struct fh_tone_profile_plan p,before;memset(&p,0x55,sizeof p);before=p;
 assert(fh_tone_profile_make_plan(63,&p)==-EINVAL&&!memcmp(&p,&before,sizeof p));
 assert(fh_tone_profile_make_plan(1025,&p)==-EINVAL&&!memcmp(&p,&before,sizeof p));
 assert(fh_tone_profile_make_plan(64,NULL)==-EINVAL);
 for(unsigned gain=64;gain<=1024;gain++){
  assert(fh_tone_profile_make_plan(gain,&p)==0&&p.count==44);
  assert(p.curve_index>=16&&p.curve_index<=20&&!p.secondary_index);
  for(unsigned i=0;i<p.count;i++){
   const struct fh_tone_profile_write *w=&p.writes[i];assert(!(w->offset&3)&&w->offset<=0x30c);
   assert(!(w->keep_mask&w->value));
   if(w->offset==0x23c)assert(w->keep_mask==0xffffff00);
   assert(w->offset<0x240||w->offset>0x250);
   for(unsigned j=0;j<i;j++)assert(p.writes[j].offset!=w->offset);
  }
 }
 puts("Static day tone:961 gains,44 unique bounded writes, adaptive fields preserved PASS");
}
