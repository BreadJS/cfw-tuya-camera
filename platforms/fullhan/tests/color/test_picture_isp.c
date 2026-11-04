#include "../../src/native/isp/picture_isp.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static uint32_t reg[0x600/4];static unsigned reads,writes,fail_read,fail_write,mismatch;static int denied;
static int owner(void *p){(void)p;return denied;}
int fh_linux_io_isp_read(void *p,uint32_t off,uint32_t *out){
 (void)p;assert(off<sizeof reg&&!(off&3));if(++reads==fail_read)return -EIO;
 *out=reg[off/4];if(reads==mismatch)*out^=off==0x4f8||off==0x4dc?0x10000:off==0x24?0x800000:1;return 0;
}
int fh_linux_io_isp_write(void *p,uint32_t off,uint32_t value){(void)p;assert(off<sizeof reg&&!(off&3));if(++writes==fail_write)return -EIO;reg[off/4]=value;return 0;}
static void reset(void){for(unsigned i=0;i<sizeof reg/sizeof *reg;i++)reg[i]=0xa5a5a5a5u;reads=writes=fail_read=fail_write=mismatch=0;denied=0;}
int main(void){
 struct fh_picture_isp_config cfg;fh_picture_isp_defaults(&cfg);assert(fh_picture_isp_valid(&cfg));
 struct fh_picture_isp_plan plan,old;assert(!fh_picture_isp_make_plan(&cfg,&plan)&&plan.count==10);old=plan;
 cfg.luma_offset=-129;assert(!fh_picture_isp_valid(&cfg)&&fh_picture_isp_make_plan(&cfg,&plan)==-EINVAL&&!memcmp(&plan,&old,sizeof plan));fh_picture_isp_defaults(&cfg);
 cfg.hue_degrees=181;assert(!fh_picture_isp_valid(&cfg));cfg.hue_degrees=-181;assert(!fh_picture_isp_valid(&cfg));fh_picture_isp_defaults(&cfg);
 struct fh_linux_io io={.check=owner};int fault=0;reset();denied=-EACCES;assert(fh_picture_isp_publish(&io,&plan,&fault)==-EACCES&&!reads&&!writes&&!fault);
 reset();assert(!fh_picture_isp_publish(&io,&plan,&fault)&&reads==20&&writes==10);
 for(unsigned i=0;i<10;i++)assert(reg[plan.writes[i].offset/4]==((0xa5a5a5a5u&plan.writes[i].keep_mask)|plan.writes[i].value));
 assert((reg[0x5d0/4]&1023)==320&&(reg[0x5c8/4]&1023)==60);
 assert((reg[0x4dc/4]&65535)==0xa5a5); /* AWB/luma coefficient untouched. */
 assert((reg[0x24/4]&0x800000)!=0&&(reg[0x24/4]&0x40000)==(0xa5a5a5a5u&0x40000));
 for(unsigned kind=0;kind<3;kind++)for(unsigned n=1;n<=(kind==0?10:20);n++){
  if(kind==2&&(n&1))continue;
  reset();fault=0;if(!kind)fail_write=n;else if(kind==1)fail_read=n;else mismatch=n;
  assert(fh_picture_isp_publish(&io,&plan,&fault)==(kind==2?-EPROTO:-EIO)&&fault);
  unsigned before=writes;assert(fh_picture_isp_publish(&io,&plan,&fault)==-EPIPE&&writes==before);
 }
 reset();fault=0;plan.writes[0].offset=0x18;assert(fh_picture_isp_publish(&io,&plan,&fault)==-EINVAL&&!writes&&!reads);plan=old;
 for(unsigned d=0;d<=200;d++)for(int hue=-180;hue<=180;hue++){
  cfg.detail_percent=d;cfg.hue_degrees=hue;assert(!fh_picture_isp_make_plan(&cfg,&plan));
  for(unsigned i=0;i<10;i++)assert(!(plan.writes[i].value&plan.writes[i].keep_mask));
 }
 puts("Picture ISP: defaults/bounds/full hue-detail ranges, masks, owner denial,10 write/20 read/10 mismatch failures PASS");return 0;
}
