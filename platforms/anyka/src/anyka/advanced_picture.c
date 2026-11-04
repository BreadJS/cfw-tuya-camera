/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "advanced_picture.h"
#include "advanced_picture_abi.h"
#include "picture_abi.h"
#include "sensor_timing.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef int (*transfer)(int,void *);
struct anyka_advanced {
    transfer ae_get,ae_set,me_get,me_set,exposure_get,exposure_set;
    transfer wb_get,wb_set,mwb_get,mwb_set,nr_get,nr_set,gamma_get,gamma_set;
    unsigned max_lines;
    struct anyka_sensor_timing timing;
    struct anyka_advanced_nr baseline_nr;
    struct anyka_advanced_rgb_gamma baseline_gamma;
};
void anyka_advanced_defaults(struct anyka_advanced_settings *s)
{if(s)*s=(struct anyka_advanced_settings){0,1,60000,100,10000,256,256,4032,1,256,256,256,1,0};}
int anyka_advanced_valid(const struct anyka_advanced_settings *s)
{
    if(!s||s->configured<0||s->configured>1||s->auto_exposure<0||s->auto_exposure>1||s->auto_white_balance<0||s->auto_white_balance>1||s->temporal_nr<0||s->temporal_nr>1||s->gamma_lift<-100||s->gamma_lift>100)return 0;
    if(s->min_exposure_us<100||s->max_exposure_us>66000||s->min_exposure_us>s->max_exposure_us||s->exposure_us<100||s->exposure_us>66000)return 0;
    if(s->min_gain_q8<256||s->max_gain_q8>4032||s->min_gain_q8>s->max_gain_q8||s->gain_q8<256||s->gain_q8>4032)return 0;
    return s->wb_red_q8>=64&&s->wb_red_q8<=4095&&s->wb_green_q8>=64&&s->wb_green_q8<=4095&&s->wb_blue_q8>=64&&s->wb_blue_q8<=4095;
}
/* Kernel copy_to_user encountering an ABI size mismatch cannot write beyond
 * the mapped output page; it returns EFAULT into the SDK wrapper instead. */
static int guarded_get(transfer fn,void *out,size_t bytes)
{
    long page=sysconf(_SC_PAGESIZE);if(!fn||!out||page<4096||page>65536||bytes+16>(size_t)page)return -EINVAL;
    unsigned char *base=mmap(NULL,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(base==MAP_FAILED)return -errno;
    if(mprotect(base+page,(size_t)page,PROT_READ|PROT_WRITE)){int rc=-errno;munmap(base,(size_t)page*3);return rc;}
    unsigned char *data=base+page*2-bytes;memset(data-16,0xa5,bytes+16);
    int rc=fn(0,data);
    for(unsigned i=0;i<16;i++)if(data[-16+(int)i]!=0xa5)rc=-EOVERFLOW;
    if(!rc)memcpy(out,data,bytes);
    munmap(base,(size_t)page*3);return rc?rc<0?rc:-EIO:0;
}
static uint32_t lines(unsigned us)
{uint32_t n=(uint32_t)(((uint64_t)us*ANYKA_SENSOR_LINES_PER_SECOND+500000)/1000000);if(!n)n=1;if(n>ANYKA_SENSOR_MAX_EXPOSURE_LINES)n=ANYKA_SENSOR_MAX_EXPOSURE_LINES;return n;}
unsigned anyka_advanced_max_us(unsigned fps)
{return fps==25?39964u:66000u;}
void anyka_advanced_clamp(struct anyka_advanced_settings *s,unsigned fps)
{
    if(!s)return;
    int max=(int)anyka_advanced_max_us(fps);
    if(s->max_exposure_us>max)s->max_exposure_us=max;
    if(s->min_exposure_us>s->max_exposure_us)s->min_exposure_us=s->max_exposure_us;
    if(s->exposure_us>max)s->exposure_us=max;
}
static uint32_t bounded_lines(struct anyka_advanced *a,unsigned us)
{uint32_t n=lines(us);return a->max_lines&&n>a->max_lines?a->max_lines:n;}
int anyka_advanced_limit_fps(struct anyka_advanced *a,unsigned fps)
{
    if(!a||(fps!=15&&fps!=25))return -EINVAL;
    int timing_rc=anyka_sensor_timing_apply(&a->timing,fps);if(timing_rc)return timing_rc;
    unsigned maximum=ANYKA_SENSOR_LINES_PER_SECOND/fps-1u;
    struct anyka_advanced_ae ae;struct anyka_advanced_me me;
    int rc=guarded_get(a->ae_get,&ae,sizeof ae);if(rc)return rc;
    if(ae.exp_time_max>maximum||ae.exp_time_min>maximum){
        if(ae.exp_time_max>maximum)ae.exp_time_max=maximum;
        if(ae.exp_time_min>maximum)ae.exp_time_min=maximum;
        rc=a->ae_set(0,&ae);if(rc)return rc<0?rc:-EIO;
        struct anyka_advanced_ae actual;rc=guarded_get(a->ae_get,&actual,sizeof actual);if(rc)return rc;
        if(actual.exp_time_max>maximum||actual.exp_time_min>maximum)return -EIO;
    }
    rc=guarded_get(a->me_get,&me,sizeof me);if(rc)return rc;
    if(me.exposure_lines>maximum){me.exposure_lines=maximum;rc=a->me_set(0,&me);if(rc)return rc<0?rc:-EIO;struct anyka_advanced_me actual;rc=guarded_get(a->me_get,&actual,sizeof actual);if(rc)return rc;if(actual.exposure_lines>maximum)return -EIO;}
    a->max_lines=maximum;return 0;
}
static unsigned square_root(unsigned n)
{unsigned lo=0,hi=1024;while(lo+1<hi){unsigned mid=(lo+hi)/2;if(mid*mid<=n)lo=mid;else hi=mid;}return lo;}
static uint16_t gamma_value(uint16_t value,int lift)
{unsigned v=value>1023?1023:value;unsigned target=lift>0?square_root(v*1023):v*v/1023;unsigned amount=(unsigned)(lift<0?-lift:lift);return(uint16_t)(((100-amount)*v+amount*target+50)/100);}
int anyka_advanced_rebaseline(struct anyka_advanced *a)
{if(!a)return -EINVAL;int rc=guarded_get(a->nr_get,&a->baseline_nr,sizeof a->baseline_nr);if(!rc)rc=guarded_get(a->gamma_get,&a->baseline_gamma,sizeof a->baseline_gamma);return rc;}
int anyka_advanced_open(struct anyka_advanced **out)
{
    if(!out)return -EINVAL;
    *out=NULL;
    const char *(*version)(void)=NULL;*(void **)&version=dlsym(RTLD_DEFAULT,"ak_vpss_get_version");
    if(!version||!version()||strcmp(version(),ANYKA_PICTURE_VPSS_VERSION))return -EPROTO;
    struct anyka_advanced *a=calloc(1,sizeof *a);if(!a)return -ENOMEM;
#define BIND(field,symbol) do {*(void **)&a->field=dlsym(RTLD_DEFAULT,symbol);if(!a->field){free(a);return -ENOSYS;}} while(0)
    BIND(ae_get,"ak_vpss_get_ae_attr");BIND(ae_set,"ak_vpss_set_ae_attr");
    BIND(me_get,"ak_vpss_get_me_attr");BIND(me_set,"ak_vpss_set_me_attr");
    BIND(exposure_get,"ak_vpss_get_exp_type");BIND(exposure_set,"ak_vpss_set_exp_type");
    BIND(wb_get,"ak_vpss_get_wb_type");BIND(wb_set,"ak_vpss_set_wb_type");
    BIND(mwb_get,"ak_vpss_get_mwb_attr");BIND(mwb_set,"ak_vpss_set_mwb_attr");
    BIND(nr_get,"ak_vpss_get_3d_nr_attr");BIND(nr_set,"ak_vpss_set_3d_nr_attr");
    BIND(gamma_get,"AK_ISP_get_rgb_gamma_attr");BIND(gamma_set,"AK_ISP_set_rgb_gamma_attr");
#undef BIND
    int rc=anyka_sensor_timing_bind(&a->timing);if(!rc)rc=anyka_advanced_rebaseline(a);if(rc){free(a);return rc;}
    *out=a;return 0;
}
static int set_checked(transfer set,transfer get,void *data,size_t bytes)
{int rc=set(0,data);if(rc)return rc<0?rc:-EIO;unsigned char copy[3724];if(bytes>sizeof copy)return -EOVERFLOW;rc=guarded_get(get,copy,bytes);if(rc)return rc;return memcmp(data,copy,bytes)?-EIO:0;}
int anyka_advanced_apply(struct anyka_advanced *a,const struct anyka_advanced_settings *s)
{
    if(!a||!anyka_advanced_valid(s))return -EINVAL;
    if(!s->configured)return 0;
    struct anyka_advanced_ae ae;struct anyka_advanced_me me;struct anyka_advanced_mwb mwb;uint16_t mode;
    int rc=guarded_get(a->ae_get,&ae,sizeof ae);if(rc)return rc;
    ae.exp_time_max=bounded_lines(a,(unsigned)s->max_exposure_us);ae.exp_time_min=bounded_lines(a,(unsigned)s->min_exposure_us);
    ae.a_gain_min=(uint32_t)s->min_gain_q8;ae.a_gain_max=(uint32_t)s->max_gain_q8;
    if((rc=a->ae_set(0,&ae)))return rc<0?rc:-EIO;
    struct anyka_advanced_ae confirmed_ae;rc=guarded_get(a->ae_get,&confirmed_ae,sizeof confirmed_ae);if(rc)return rc;
    if(confirmed_ae.exp_time_max!=ae.exp_time_max||confirmed_ae.exp_time_min!=ae.exp_time_min||confirmed_ae.a_gain_min!=ae.a_gain_min||confirmed_ae.a_gain_max!=ae.a_gain_max)return -EIO;
    if(!s->auto_exposure){rc=guarded_get(a->me_get,&me,sizeof me);if(rc)return rc;me.exposure_lines=bounded_lines(a,(unsigned)s->exposure_us);me.analog_gain_q8=(uint32_t)s->gain_q8;if((rc=a->me_set(0,&me)))return rc<0?rc:-EIO;struct anyka_advanced_me confirmed_me;rc=guarded_get(a->me_get,&confirmed_me,sizeof confirmed_me);if(rc)return rc;if(confirmed_me.exposure_lines!=me.exposure_lines||confirmed_me.analog_gain_q8!=me.analog_gain_q8)return -EIO;}
    mode=(uint16_t)(s->auto_exposure?1:0);if((rc=set_checked(a->exposure_set,a->exposure_get,&mode,sizeof mode)))return rc;
    if(!s->auto_white_balance){rc=guarded_get(a->mwb_get,&mwb,sizeof mwb);if(rc)return rc;mwb.red_q8=(uint16_t)s->wb_red_q8;mwb.green_q8=(uint16_t)s->wb_green_q8;mwb.blue_q8=(uint16_t)s->wb_blue_q8;if((rc=set_checked(a->mwb_set,a->mwb_get,&mwb,sizeof mwb)))return rc;}
    mode=(uint16_t)(s->auto_white_balance?1:0);if((rc=set_checked(a->wb_set,a->wb_get,&mode,sizeof mode)))return rc;
    struct anyka_advanced_nr nr=a->baseline_nr;
    for(unsigned i=0;i<10;i++){nr.profile[i][0]=(uint8_t)s->temporal_nr;nr.profile[i][1]=0;}
    if((rc=set_checked(a->nr_set,a->nr_get,&nr,sizeof nr)))return rc;
    struct anyka_advanced_rgb_gamma gamma=a->baseline_gamma;
    for(unsigned i=0;i<129;i++){gamma.red[i]=gamma_value(gamma.red[i],s->gamma_lift);gamma.green[i]=gamma_value(gamma.green[i],s->gamma_lift);gamma.blue[i]=gamma_value(gamma.blue[i],s->gamma_lift);}
    return set_checked(a->gamma_set,a->gamma_get,&gamma,sizeof gamma);
}
