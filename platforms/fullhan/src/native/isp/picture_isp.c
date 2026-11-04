#include "picture_isp.h"
#include "picture_sine.h"
#include <errno.h>
static const uint32_t offsets[FH_PICTURE_ISP_WRITES]={0x5c8,0x5cc,0x5d0,0x4dc,0x4e0,0x4e4,0x4e8,0x24,0x4f8,0x4fc};
static const uint32_t masks[FH_PICTURE_ISP_WRITES]={0xff00fc00,0xfe00fe00,0xfffffc00,0xf000ffff,0xf000f000,0xf000f000,0xfffff000,0xff7fffff,0x0000ffff,0};
void fh_picture_isp_defaults(struct fh_picture_isp_config *out)
{if(out)*out=(struct fh_picture_isp_config){320,60,0,0,54,50,0,100};}
int fh_picture_isp_valid(const struct fh_picture_isp_config *c)
{return c&&c->saturation<=1020&&c->contrast<=255&&c->luma_offset>=-128&&c->luma_offset<=127&&c->hue_degrees>=-180&&c->hue_degrees<=180&&c->cb_gain<=200&&c->cr_gain<=200&&c->temporal_nr<=1&&c->detail_percent<=200;}
/* Defined arithmetic right-shift equivalent, including negative coefficients. */
static int shift6(int n){return n>=0?n/64:-((-n+63)/64);}
int fh_picture_isp_make_plan(const struct fh_picture_isp_config *c,struct fh_picture_isp_plan *out)
{
 if(!out||!fh_picture_isp_valid(c))return -EINVAL;
 unsigned angle=(unsigned)((c->hue_degrees+360)%360),cosine=(angle+90)%360;
 uint32_t values[FH_PICTURE_ISP_WRITES]={
  c->contrast|((uint32_t)c->luma_offset&255u)<<16,
  ((uint32_t)camera_picture_sine[angle]&511u)|((uint32_t)camera_picture_sine[cosine]&511u)<<16,
  c->saturation,
  (uint32_t)((int)c->cb_gain*-0x2b400)&0x0fff0000u,
  (c->cb_gain<<19)|((uint32_t)shift6((int)c->cb_gain*-0x153)&4095u),
  ((uint32_t)((int)c->cr_gain*-0x6b400)&0x0fff0000u)|((c->cr_gain*0x200u)>>6),
  (uint32_t)shift6((int)c->cr_gain*-0x53)&4095u,
  c->temporal_nr?0:UINT32_C(0x800000),
  ((20u*c->detail_percent+50u)/100u)<<16,
  ((16u*c->detail_percent+50u)/100u)*UINT32_C(0x10001)
 };
 struct fh_picture_isp_plan plan={.count=FH_PICTURE_ISP_WRITES};
 for(unsigned i=0;i<FH_PICTURE_ISP_WRITES;i++)plan.writes[i]=(struct fh_picture_isp_write){offsets[i],masks[i],values[i]};
 *out=plan;return 0;
}
int fh_picture_isp_publish(struct fh_linux_io *io,const struct fh_picture_isp_plan *p,int *fault)
{
 if(!io||!io->check||!p||!fault||p->count!=FH_PICTURE_ISP_WRITES)return -EINVAL;
 if(*fault)return -EPIPE;
 for(unsigned i=0;i<FH_PICTURE_ISP_WRITES;i++)if(p->writes[i].offset!=offsets[i]||p->writes[i].keep_mask!=masks[i]||(p->writes[i].value&masks[i]))return -EINVAL;
 int rc=io->check(io->owner);if(rc)return rc<0?rc:-EPERM;
 for(unsigned i=0;i<FH_PICTURE_ISP_WRITES;i++){
  const struct fh_picture_isp_write *w=&p->writes[i];uint32_t old,after;
  rc=fh_linux_io_isp_read(io,w->offset,&old);if(rc)goto failed;
  uint32_t value=(old&w->keep_mask)|w->value;
  rc=fh_linux_io_isp_write(io,w->offset,value);if(rc)goto failed;
  rc=fh_linux_io_isp_read(io,w->offset,&after);if(rc)goto failed;
  if((after^value)&~w->keep_mask){rc=-EPROTO;goto failed;}
 }
 return 0;
failed:*fault=rc<0?rc:-EIO;return *fault;
}
