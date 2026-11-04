#include "isp_stats.h"
#include "isp_shadow.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static uint32_t get(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int fail(struct fh_isp_stats *s,int rc)
{s->faulted=1;s->last_error=rc<0?rc:-EPROTO;return s->last_error;}
static int check(struct fh_isp_stats *s)
{int rc=s->io.check(s->io.opaque);return rc?fail(s,rc):0;}
static int call(struct fh_isp_stats *s,uint32_t cmd,uint8_t *wire,size_t n,int refresh)
{
    int rc=check(s);if(rc)return rc;
    rc=s->io.ioctl(s->io.opaque,s->fd,cmd,wire,n);
    if(rc<0)return fail(s,rc);
    if(refresh)return rc>0?0:-EAGAIN;
    return rc?fail(s,-EPROTO):0;
}
static uint32_t root(uint32_t x)
{
    uint32_t value=0,bit=UINT32_C(1)<<30;
    while(bit>x)bit>>=2;
    while(bit) {if(x>=value+bit){x-=value+bit;value=(value>>1)+bit;}else value>>=1;bit>>=2;}
    return value;
}
int fh_isp_stats_init(struct fh_isp_stats *s,const struct fh_isp_memory *memory,
                      int fd,const struct fh_isp_stats_io *io)
{
    if(!s||!memory||fd<0||!io||!io->check||!io->ioctl||!io->read32)return -EINVAL;
    struct fh_isp_region bank;int rc=fh_isp_memory_stat_bank(memory,0,&bank);
    if(rc)return rc;
    if(!bank.user||(bank.user&3))return -EINVAL;
    memset(s,0,sizeof *s);s->memory=*memory;s->fd=fd;s->io=*io;s->initialized=1;return 0;
}
int fh_isp_stats_read(struct fh_isp_stats *s,unsigned timeout_ms,
                      struct fh_isp_stats_sample *out)
{
    if(!s||!out||!s->initialized||!timeout_ms||timeout_ms>100)return -EINVAL;
    if(s->faulted)return -EPIPE;
    uint32_t grid;int rc=check(s);if(rc)return rc;
    rc=s->io.read32(s->io.opaque,0x1cc,&grid);if(rc)return fail(s,rc);
    if(((grid>>16)&31)!=15||((grid>>24)&31)!=15)return fail(s,-ENOTSUP);
    uint32_t geometry;
    rc=s->io.read32(s->io.opaque,0x30,&geometry);if(rc)return fail(s,rc);
    uint8_t wait[4]={(uint8_t)timeout_ms,0,0,0},status[16]={0},offset[4]={0};
    rc=call(s,FH_ISP_STATS_REFRESH,wait,4,1);if(rc)return rc;
    rc=call(s,FH_ISP_STATS_STATUS,status,16,0);if(rc)return rc;
    uint32_t counter=get(status);
    rc=call(s,FH_ISP_STATS_OFFSET,offset,4,0);if(rc)return rc;
    uint32_t selected=get(offset);
    struct fh_isp_region bank;
    rc=fh_isp_memory_stat_bank(&s->memory,selected,&bank);
    if(rc)return selected==UINT32_MAX||selected==(uint32_t)-FH_ISP_STAT_BANK_BYTES?-EAGAIN:fail(s,rc);
    if(!counter)return -EAGAIN;
    if(s->have_previous) {
        uint32_t delta=counter-s->previous_counter;
        if(!delta||delta>=UINT32_C(0x80000000))return -EAGAIN;
    }
    /* Copy only the two recovered AE words of each16-byte record. Completed
     * bank stays pinned until this exclusive owner requests another refresh.
     * No invented userspace pointer from an ioctl is dereferenced. */
    uint32_t saved[4*256*2];
    const volatile uint32_t *base=(const volatile uint32_t *)(uintptr_t)(bank.user+0x5c8);
    rc=check(s);if(rc)return rc;
    __sync_synchronize();
    for(unsigned plane=0;plane<4;plane++)for(unsigned tile=0;tile<256;tile++) {
        unsigned i=(plane*256+tile)*2,j=plane*1024+tile*4;
        saved[i]=base[j];saved[i+1]=base[j+1];
    }
    struct fh_awb_sample awb={0};
    const volatile uint32_t *rgb=(const volatile uint32_t *)(uintptr_t)(bank.user+0x48);
    for(unsigned i=0;i<FH_AWB_CELLS;i++)awb.cell[i]=(struct fh_awb_cell){rgb[4*i],rgb[4*i+1],rgb[4*i+2],rgb[4*i+3]};
    const volatile uint32_t *hist=(const volatile uint32_t *)(uintptr_t)(bank.user+0x21250);
    uint32_t histogram[3]={hist[0],hist[1],hist[2]};
    __sync_synchronize();
    unsigned awb_valid=1;
    for(unsigned i=0;i<FH_AWB_CELLS;i++){
        const struct fh_awb_cell *c=&awb.cell[i];
        if(c->red!=rgb[4*i]||c->green!=rgb[4*i+1]||c->blue!=rgb[4*i+2]||c->count!=rgb[4*i+3])return -EAGAIN;
        if((uint64_t)c->red>(uint64_t)c->count*4096u||(uint64_t)c->green>(uint64_t)c->count*4096u||(uint64_t)c->blue>(uint64_t)c->count*4096u)awb_valid=0;
    }
    uint64_t sum=0,count=0;
    for(unsigned plane=0;plane<4;plane++)for(unsigned tile=0;tile<256;tile++) {
        unsigned i=(plane*256+tile)*2,j=plane*1024+tile*4;
        if(saved[i]!=base[j]||saved[i+1]!=base[j+1])return -EAGAIN;
        if(!saved[i+1]) {if(saved[i])return fail(s,-ERANGE);continue;}
        if((uint64_t)saved[i]>(uint64_t)saved[i+1]*4096)return fail(s,-ERANGE);
        sum+=saved[i];count+=saved[i+1];
    }
    __sync_synchronize();
    for(unsigned i=0;i<3;i++)if(histogram[i]!=hist[i])return -EAGAIN;
    rc=call(s,FH_ISP_STATS_OFFSET,offset,4,0);if(rc)return rc;
    if(get(offset)!=selected)return -EAGAIN;
    rc=call(s,FH_ISP_STATS_STATUS,status,16,0);if(rc)return rc;
    if(get(status)!=counter)return -EAGAIN;
    rc=check(s);if(rc)return rc;
    if(!count)return -EAGAIN;
    if(s->generation==UINT64_MAX)return fail(s,-EOVERFLOW);
    uint32_t mean=(uint32_t)(sum/count);
    struct fh_isp_stats_sample result={.generation=s->generation+1,.sum=sum,.count=count,
        .bank_offset=selected,.source_irq_counter=counter,
        .raw_mean_q16=(uint32_t)(sum*65535/(count*4096)),
        .mean_q16=(uint32_t)((uint64_t)root(mean<<12)*65535/4096)};
    awb.generation=result.generation;result.awb=awb;result.awb_valid=awb_valid;
    result.shadow_width=geometry&2047;result.shadow_height=(geometry>>16)&2047;
    memcpy(result.shadow_histogram,histogram,sizeof histogram);
    struct fh_shadow_tile tiles[255];
    for(unsigned i=0;i<255;i++){
        for(unsigned p=0;p<4;p++)tiles[i].sum[p]=saved[(p*256+i)*2];
        tiles[i].count=saved[i*2+1];
    }
    uint32_t area=result.shadow_width*result.shadow_height;
    result.shadow_valid=area>=64&&histogram[2]<=65535&&histogram[0]/(area>>6)<=65535&&
        !fh_shadow_extrema(tiles,255,&result.shadow_min_log_q8,&result.shadow_max_log_q8)&&
        result.shadow_min_log_q8<=8192&&result.shadow_max_log_q8<=8192;
    s->generation=result.generation;s->have_previous=1;s->previous_counter=counter;
    s->previous_offset=selected;*out=result;return 0;
}
int fh_isp_stats_linux_ioctl(void *opaque,int fd,uint32_t cmd,uint8_t *wire,size_t bytes)
{
    (void)opaque;
    if(fd<0||!wire||((cmd==FH_ISP_STATS_STATUS)?bytes!=16:
        (cmd==FH_ISP_STATS_OFFSET||cmd==FH_ISP_STATS_REFRESH)?bytes!=4:1))return -EINVAL;
    int rc=ioctl(fd,(unsigned long)cmd,wire);return rc==-1?-errno:rc;
}
