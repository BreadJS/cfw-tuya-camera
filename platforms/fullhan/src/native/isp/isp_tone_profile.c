#include "isp_tone_profile.h"
#include "isp_tone_profile_data.h"
#include <errno.h>
#include <string.h>
/*107520 +1074d0: gain-power bucket, then original signed roundedQ16
 * interpolation. Mode0 reads packed nibbles, mode1 reads bytes. */
static unsigned lookup(unsigned offset,unsigned gain,int byte_mode){
 unsigned power=0;for(unsigned g=gain;g>1;g>>=1)power++;
 unsigned index=power-6,base=1u<<power;const uint8_t *p=camera_tone_profile_bytes+offset-0x260;
 int a,b;if(byte_mode){a=p[index];b=p[index+1];}else{a=(p[index/2]>>((index&1)*4))&15;b=(p[(index+1)/2]>>(((index+1)&1)*4))&15;}
 int slope=(b-a)*65536/(int)base;
 int delta=((int)gain-(int)base)*slope+(slope>=0?32768:-32768);
 return (unsigned)(a+delta/65536);
}
static void add(struct fh_tone_profile_plan *p,uint32_t offset,uint32_t mask,uint32_t value){p->writes[p->count++]=(struct fh_tone_profile_write){offset,mask,value};}
int fh_tone_profile_make_plan(unsigned gain,struct fh_tone_profile_plan *out){
 if(!out||gain<64||gain>1024)return -EINVAL;
 struct fh_tone_profile_plan plan={0};const uint8_t *p=camera_tone_profile_bytes;
 unsigned header=p[0],kind=(header>>2)&3;if(kind>1)kind=2;
 unsigned curve,secondary,low,high,amount;
 if(header&1){curve=lookup(0x290,gain,1)&255;secondary=lookup(0x29c,gain,0)&255;low=lookup(0x278,gain,1);high=lookup(0x284,gain,1);amount=lookup(0x26c,gain,1);}
 else{curve=p[1]&31;secondary=header>>4;low=p[2];high=p[3];amount=p[6];}
 if(curve>23)curve=24;
 if(secondary>8)secondary=9;
 uint32_t reg254;
 if(!kind){if(high<low)return -ERANGE;reg254=((uint32_t)(high-low)*16*0x200000u)|((high&255)<<16)|((low*16)&0xff0);}
 else reg254=kind==1?0x00ffffff:0xff000000;
 add(&plan,0x24,0xfffbffff,(header&2)?0:0x40000);
 add(&plan,0x254,0,reg254);
 add(&plan,0x23c,0xffffff00,p[4]);
 add(&plan,0x258,0xf00fffff,(amount&255)<<20);
 /* Exhaustive supported gain lookup reaches only these exact-profile rows. */
 if(curve<16||curve>20||secondary)return -ERANGE;
 const uint16_t *a=camera_tone_curve[curve-16],*b=camera_tone_secondary[0];
 for(unsigned i=0;i<18;i++)add(&plan,0x270+i*4,0,a[2*i]|(uint32_t)a[2*i+1]<<16);
 add(&plan,0x2b8,0,a[36]|(uint32_t)a[37]<<16);
 add(&plan,0x2bc,0,a[38]|(uint32_t)a[39]<<16);
 add(&plan,0x2c0,0,a[40]);
 for(unsigned i=0;i<18;i++)add(&plan,0x2c4+i*4,0,b[2*i]|(uint32_t)b[2*i+1]<<16);
 add(&plan,0x30c,0,b[36]);
 plan.curve_index=curve;plan.secondary_index=secondary;
 *out=plan;return 0;
}
