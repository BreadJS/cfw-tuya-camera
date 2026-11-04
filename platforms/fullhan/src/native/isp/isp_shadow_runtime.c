#include "isp_shadow_runtime.h"
#include "isp_tone_profile.h"
#include <errno.h>
static int failed(struct fh_shadow_runtime *s,int rc){s->fault=rc<0?rc:-EIO;return s->fault;}
int fh_shadow_update(struct fh_linux_io *io,struct fh_shadow_runtime *s,const struct fh_isp_stats_sample *sample,unsigned gain)
{const struct fh_shadow_config cfg=FH_SHADOW_DAY_CONFIG;return fh_shadow_update_config(io,s,sample,gain,&cfg);}
int fh_shadow_update_config(struct fh_linux_io *io,struct fh_shadow_runtime *s,const struct fh_isp_stats_sample *sample,unsigned gain,const struct fh_shadow_config *cfg)
{
 if(!io||!s||!sample||!cfg)return -EINVAL;
 if(s->fault)return -EPIPE;
 if(!sample->shadow_valid)return 0;
 if(sample->generation<=s->state.generation)return -ESTALE;
 struct fh_tone_profile_plan tone;int rc=fh_tone_profile_make_plan(gain,&tone);if(rc)return rc;
 if(tone.count>FH_TONE_PROFILE_MAX_WRITES)return -EOVERFLOW;
 struct fh_shadow_input in={.generation=sample->generation,.width=sample->shadow_width,.height=sample->shadow_height,
  .histogram={sample->shadow_histogram[0],sample->shadow_histogram[1],sample->shadow_histogram[2]},
  .min_log_q8=sample->shadow_min_log_q8,.max_log_q8=sample->shadow_max_log_q8};
 const uint32_t offsets[6]={0x23c,0x240,0x244,0x248,0x24c,0x250};uint32_t old[6];
 for(unsigned i=0;i<6;i++){rc=fh_linux_io_isp_read(io,offsets[i],&old[i]);if(rc)return rc;}
 /* Compute before publication so a rejected input cannot partially update
  * tables. Preserve the static profile's low23c byte in the adaptive plan. */
 for(size_t i=0;i<tone.count;i++)if(tone.writes[i].offset==0x23c)old[0]=(old[0]&tone.writes[i].keep_mask)|tone.writes[i].value;
 in.reg23c=old[0];in.reg240=old[1];in.reg244=old[2];in.reg248=old[3];in.reg24c=old[4];in.reg250=old[5];
 struct fh_shadow_state proposed=s->state;struct fh_shadow_plan plan;
 rc=fh_shadow_make_plan(cfg,&proposed,&in,&plan);if(rc)return rc;
 struct fh_tone_profile_write enable={0};unsigned have_enable=0;
 for(size_t i=0;i<tone.count;i++){
  const struct fh_tone_profile_write *w=&tone.writes[i];
  if(w->offset==0x24){enable=*w;have_enable=1;continue;}
  uint32_t value=w->value;
  if(w->keep_mask){uint32_t current;rc=fh_linux_io_isp_read(io,w->offset,&current);if(rc)return failed(s,rc);value|=current&w->keep_mask;}
  rc=fh_linux_io_isp_write(io,w->offset,value);if(rc)return failed(s,rc);
 }
 uint32_t values[6]={plan.reg23c,plan.reg240,plan.reg244,plan.reg248,plan.reg24c,plan.reg250};
 for(unsigned i=0;i<6;i++){rc=fh_linux_io_isp_write(io,offsets[i],values[i]);if(rc)return failed(s,rc);}
 if(have_enable){uint32_t current;rc=fh_linux_io_isp_read(io,0x24,&current);if(rc)return failed(s,rc);rc=fh_linux_io_isp_write(io,0x24,(current&enable.keep_mask)|enable.value);if(rc)return failed(s,rc);}
 s->state=proposed;return 1;
}
