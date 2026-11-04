#include "../../src/anyka/advanced_picture.c"
#include <assert.h>
#include <stdio.h>
static struct anyka_advanced_ae ae;
static struct anyka_advanced_me me;
static struct anyka_advanced_mwb mwb;
static struct anyka_advanced_nr nr;
static struct anyka_advanced_rgb_gamma gamma;
static uint16_t exposure_mode=1,wb_mode=1;
static int sensor_fps=15;
static uint32_t fps_levels[9]={15,1874,16,0,0,0,15,1874,8};
static int sensor_get(int dev){assert(!dev);return sensor_fps;}
static int sensor_set(int dev,void *in){assert(!dev);sensor_fps=*(int*)in;return 0;}
static int levels_get(int dev,void *out){assert(!dev);memcpy(out,fps_levels,sizeof fps_levels);return 0;}
static int levels_set(int dev,void *in){assert(!dev);memcpy(fps_levels,in,sizeof fps_levels);return 0;}
static unsigned writes;
static int set_error,corrupt_readback;
#define MOCK(name,type,object) static int name##_get(int dev,void *out){assert(!dev);memcpy(out,&object,sizeof(type));return 0;} static int name##_set(int dev,void *in){assert(!dev);writes++;if(set_error)return set_error;memcpy(&object,in,sizeof(type));return 0;}
MOCK(ae,struct anyka_advanced_ae,ae)
MOCK(me,struct anyka_advanced_me,me)
MOCK(mwb,struct anyka_advanced_mwb,mwb)
MOCK(nr,struct anyka_advanced_nr,nr)
MOCK(gamma,struct anyka_advanced_rgb_gamma,gamma)
static int exposure_get(int dev,void *out){assert(!dev);memcpy(out,&exposure_mode,2);if(corrupt_readback)*(uint16_t *)out=99;return 0;}
static int exposure_set(int dev,void *in){assert(!dev);writes++;memcpy(&exposure_mode,in,2);return 0;}
MOCK(wb,uint16_t,wb_mode)
static int underflow(int dev,void *out){(void)dev;((unsigned char*)out)[-1]=0;return 0;}
int main(void)
{
 struct anyka_advanced a={.timing={sensor_get,sensor_set,levels_get,levels_set},.ae_get=ae_get,.ae_set=ae_set,.me_get=me_get,.me_set=me_set,.exposure_get=exposure_get,.exposure_set=exposure_set,.wb_get=wb_get,.wb_set=wb_set,.mwb_get=mwb_get,.mwb_set=mwb_set,.nr_get=nr_get,.nr_set=nr_set,.gamma_get=gamma_get,.gamma_set=gamma_set};
 memset(&ae,0x3a,sizeof ae);memset(&me,0x2a,sizeof me);memset(&mwb,0x11,sizeof mwb);memset(&nr,0x2b,sizeof nr);
 gamma.enabled=1;for(unsigned i=0;i<129;i++)gamma.red[i]=gamma.green[i]=gamma.blue[i]=(uint16_t)(i*1023/128);
 struct anyka_advanced_settings s;anyka_advanced_defaults(&s);assert(anyka_advanced_valid(&s));assert(!anyka_advanced_apply(&a,&s)&&!writes);
 assert(!anyka_advanced_rebaseline(&a));struct anyka_advanced_ae original_ae=ae;struct anyka_advanced_nr original_nr=nr;
 s.configured=1;s.auto_exposure=0;s.exposure_us=20000;s.max_exposure_us=50000;s.gain_q8=512;s.auto_white_balance=0;s.wb_red_q8=700;s.temporal_nr=0;s.gamma_lift=50;
 assert(!anyka_advanced_apply(&a,&s));assert(me.exposure_lines==563&&me.analog_gain_q8==512);assert(ae.exp_time_max==1406&&ae.a_gain_max==4032);assert(!memcmp(ae.reserved_profile_fields,original_ae.reserved_profile_fields,sizeof ae.reserved_profile_fields));assert(ae.exp_step==original_ae.exp_step);assert(!exposure_mode&&!wb_mode&&mwb.red_q8==700&&mwb.red_offset==0x1111);
 for(unsigned i=0;i<10;i++){assert(!nr.profile[i][0]&&!nr.profile[i][1]);assert(!memcmp(nr.profile[i]+2,original_nr.profile[i]+2,370));}
 uint16_t lifted=gamma.red[32];assert(lifted>a.baseline_gamma.red[32]);assert(!anyka_advanced_apply(&a,&s)&&gamma.red[32]==lifted);
 s.gamma_lift=0;s.temporal_nr=1;s.auto_exposure=1;s.auto_white_balance=1;assert(!anyka_advanced_apply(&a,&s));assert(gamma.red[32]==a.baseline_gamma.red[32]&&exposure_mode==1&&wb_mode==1);
 corrupt_readback=1;assert(anyka_advanced_apply(&a,&s)==-EIO);corrupt_readback=0;
 unsigned char tiny;assert(guarded_get(underflow,&tiny,1)==-EOVERFLOW);
 s.max_gain_q8=65535;unsigned before=writes;assert(anyka_advanced_apply(&a,&s)==-EINVAL&&before==writes);
 assert(lines(100)==3&&lines(66000)==1856&&lines(1000000)==1874);
 ae.exp_time_max=1874;ae.exp_time_min=1200;me.exposure_lines=1800;
 assert(!anyka_advanced_limit_fps(&a,25));assert(a.max_lines==1124&&ae.exp_time_max==1124&&ae.exp_time_min==1124&&me.exposure_lines==1124);
 assert(!memcmp(ae.reserved_profile_fields,original_ae.reserved_profile_fields,sizeof ae.reserved_profile_fields));
 s.max_gain_q8=4032;s.min_exposure_us=100;s.max_exposure_us=60000;s.exposure_us=60000;s.auto_exposure=0;
 assert(!anyka_advanced_apply(&a,&s)&&ae.exp_time_max==1124&&me.exposure_lines==1124);
 anyka_advanced_clamp(&s,25);assert(s.max_exposure_us==39964&&s.exposure_us==39964&&anyka_advanced_max_us(15)==66000);
 assert(anyka_advanced_limit_fps(&a,30)==-EINVAL);
 for(int lift=-100;lift<=100;lift++){unsigned previous=0;for(unsigned i=0;i<=1023;i++){unsigned value=gamma_value((uint16_t)i,lift);assert(value>=previous&&value<=1023);previous=value;}assert(gamma_value(0,lift)==0&&gamma_value(1023,lift)==1023);}
 puts("PASS advanced picture: guarded getter buffers, exact units, preserve unknown bytes, manual/auto AE/WB, TNR profiles, noncompounding gamma, readback/error validation");
}
