#include "../../src/native/isp/isp_color_auto.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static uint32_t reg[0x600/4];static unsigned writes,fail;
int fh_linux_io_isp_read(void *p,uint32_t off,uint32_t *out){(void)p;assert(off==0x4bc);*out=reg[off/4];return 0;}
int fh_linux_io_isp_write(void *p,uint32_t off,uint32_t value){(void)p;writes++;if(writes==fail)return -EIO;assert((off>=0x18c&&off<=0x1a0)||off==0x224||off==0x228||off==0x4bc||(off>=0x4c0&&off<=0x4d4));reg[off/4]=value;return 0;}
int main(void){
 struct fh_color_plan p={0},old;struct fh_awb_gains g={512,512,512};
 for(unsigned b=0;b<4;b++){assert(!fh_color_make_plan(&g,b,0x300030,0x300030,0x2590132,0xf6e0074,&p));assert(p.gain01==p.gain23);assert((p.gain01&65535u)==516);assert(p.clip==893);}
 g=(struct fh_awb_gains){1024,512,768};
 assert(!fh_color_make_plan(&g,1,0x300030,0x300030,0x2590132,0xf6e0074,&p));
 old=p;g.red=0;assert(fh_color_make_plan(&g,1,0,0,0x2590132,116,&p)==-EINVAL&&!memcmp(&p,&old,sizeof p));
 g.red=1024;assert(fh_color_make_plan(&g,4,0,0,0x2590132,116,&p)==-EINVAL);assert(fh_color_make_plan(&g,1,0xffffffff,0xffffffff,0x2590132,116,&p)==-ERANGE);
 struct fh_linux_io io={0};int fault=0;reg[0x4bc/4]=0xabcd0000;assert(!fh_color_publish(&io,&p,&fault)&&writes==9&&!fault);assert(reg[0x4bc/4]==(0xabcd0000|p.clip));
 for(unsigned i=0;i<6;i++)assert(reg[(0x4c0+4*i)/4]==p.ccm[i]);
 for(fail=1;fail<=9;fail++){writes=0;fault=0;assert(fh_color_publish(&io,&p,&fault)==-EIO&&fault);unsigned n=writes;assert(fh_color_publish(&io,&p,&fault)==-EPIPE&&writes==n);}
 fail=0;writes=0;assert(!fh_color_statistics_profile(&io)&&writes==6);
 const uint32_t window[6]={0x00800830,0x12f7992f,0x2405d240,0x230565e2,0x1ad8c159,0x000ffbfe};
 for(unsigned i=0;i<6;i++)assert(reg[(0x18c+4*i)/4]==window[i]);
 for(fail=1;fail<=6;fail++){writes=0;assert(fh_color_statistics_profile(&io)==-EIO&&writes==fail);}
 puts("Colour plan/publish: bounds, Bayer, luma/BLC, clip, publish failures and original neutral statistics window PASS");return 0;
}
