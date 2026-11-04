#include "isp_color_auto.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
static int log_q8(unsigned x){unsigned n=0,v=x;while(v>1){v>>=1;n++;}return (int)(n*256u)+(int)(((uint64_t)(x-(1u<<n))*256u)>>n);}
struct point {int x,y;};
static struct point point(unsigned packed){unsigned a=packed&65535u,b=packed>>16,m=(a+b+8194u)/4u;return(struct point){log_q8(b)-log_q8(m),log_q8(a)-log_q8(m)};}
static int distance(struct point a,struct point b){int x=a.x-b.x,y=a.y-b.y;return x*x+y*y;}
int fh_color_make_plan(const struct fh_awb_gains *g,unsigned bayer,uint32_t blc0,uint32_t blc1,uint32_t y0,uint32_t y1,struct fh_color_plan *out){
 if(!g||!out||bayer>3||g->red<256||g->red>2048||g->green<256||g->green>2048||g->blue<256||g->blue>2048)return -EINVAL;
 unsigned a=g->green*4096u/g->blue,b=g->green*4096u/g->red;if(!a||!b||a>65535||b>65535)return -ERANGE;
 struct point p=point(a|(b<<16)),anchor[4];int d[4];unsigned sorted[4]={0,1,2,3};
 for(unsigned i=0;i<4;i++){anchor[i]=point(camera_color_anchor[i]);d[i]=distance(anchor[i],p);}
 for(unsigned i=0;i<4;i++){unsigned least=i;for(unsigned j=i+1;j<4;j++)if(d[sorted[j]]<d[sorted[least]])least=j;unsigned k=sorted[i];sorted[i]=sorted[least];sorted[least]=k;}
 unsigned first=sorted[0],second=sorted[1];int dx=anchor[second].x-anchor[first].x,dy=anchor[second].y-anchor[first].y;
 int dot=dx*(p.x-anchor[first].x)+dy*(p.y-anchor[first].y);
 if(first!=0&&first!=3&&dot<0){second=sorted[2];dx=anchor[second].x-anchor[first].x;dy=anchor[second].y-anchor[first].y;dot=dx*(p.x-anchor[first].x)+dy*(p.y-anchor[first].y);}
 int den=dx*dx+dy*dy;if(den<1)den=1;int weight=(int)((int64_t)dot*256/den);if(weight<0)weight=0;if(weight>256)weight=256;
 struct fh_color_plan plan={.first=first,.second=second,.weight=(unsigned)weight};
 for(unsigned i=0;i<6;i++)for(unsigned j=0;j<2;j++){int v=((256-weight)*camera_color_matrix[first][2*i+j]+weight*camera_color_matrix[second][2*i+j]+128)/256;plan.ccm[i]|=((uint32_t)v&8191u)<<(16*j);}
 unsigned yr=y0&4095u,yg=(y0>>16)&4095u,yb=y1&4095u;
 if(!yr||!yg||!yb||yr+yg+yb>2048)return -ERANGE;
 unsigned norm=yr*512u/g->red+yg*512u/g->green+yb*512u/g->blue;
 unsigned logical[3]={(unsigned)((uint64_t)norm*g->red/1024u),(unsigned)((uint64_t)norm*g->green/1024u),(unsigned)((uint64_t)norm*g->blue/1024u)};
 unsigned black=((blc0&4095u)+(blc0>>16&4095u)+(blc1&4095u)+(blc1>>16&4095u))/4;
 if(black>1024)return -ERANGE;
 unsigned scale=0x100000u/(4096u-black),min=UINT_MAX,pos[4]={0};
 const uint8_t *map=camera_bayer_permutation[bayer];
 for(unsigned i=0;i<3;i++){unsigned val=scale*logical[i]/256;if(!val||val>4095)return -ERANGE;pos[map[i]]=val;if(logical[i]<min)min=logical[i];}
 pos[map[3]]=pos[map[1]];plan.gain01=pos[0]|pos[1]<<16;plan.gain23=pos[2]|pos[3]<<16;
 unsigned clip=(unsigned)(((uint64_t)min*1023u/512u)*(128+CAMERA_COLOR_CLIP_BIAS)/128u);plan.clip=clip>1023?1023:clip;
 *out=plan;return 0;
}
int fh_color_statistics_profile(struct fh_linux_io *io)
{
 if(!io)return -EINVAL;
 /* Original10b094(1), day profile78/7a and b4..c4. The last word is
  * masked to20bits by the original; no unbounded vendor profile loader. */
 const uint32_t offsets[6]={0x18c,0x190,0x194,0x198,0x19c,0x1a0};
 const uint32_t values[6]={0x00800830,0x12f7992f,0x2405d240,0x230565e2,0x1ad8c159,0x000ffbfe};
 for(unsigned i=0;i<6;i++){int rc=fh_linux_io_isp_write(io,offsets[i],values[i]);if(rc)return rc;}
 return 0;
}
int fh_color_publish(struct fh_linux_io *io,const struct fh_color_plan *p,int *fault){
 if(!io||!p||!fault)return -EINVAL;
 if(*fault)return -EPIPE;
 uint32_t clip;int rc=fh_linux_io_isp_read(io,0x4bc,&clip);if(rc)return rc;
 uint32_t offsets[9]={0x224,0x228,0x4bc,0x4c0,0x4c4,0x4c8,0x4cc,0x4d0,0x4d4};
 uint32_t values[9]={p->gain01,p->gain23,(clip&0xffff0000u)|p->clip,0,0,0,0,0,0};memcpy(values+3,p->ccm,sizeof p->ccm);
 for(unsigned i=0;i<9;i++){rc=fh_linux_io_isp_write(io,offsets[i],values[i]);if(rc){*fault=1;return rc;}}
 return 0;
}
