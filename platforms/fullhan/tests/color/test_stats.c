#define _GNU_SOURCE
#include "../../src/native/isp/isp_stats.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
struct fixture {
    struct fh_isp_stats stats;
    struct fh_isp_memory memory;
    void *ram;uint32_t bytes,offset,counter,grid;
    unsigned checks,fail_at,calls,status_calls,offset_calls;
    int timeout,bad_status,bad_offset,io_error;
};
static void put(uint8_t *p,uint32_t x)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static int lease(void *p)
{struct fixture*f=p;f->checks++;return f->checks==f->fail_at?-EACCES:0;}
static int mmio(void *p,uint32_t off,uint32_t *x)
{assert(off==0x1cc||off==0x30);*x=off==0x1cc?((struct fixture*)p)->grid:((1087u<<16)|1927u);return 0;}
static int control(void *p,int fd,uint32_t cmd,uint8_t *wire,size_t n)
{
    struct fixture*f=p;assert(fd==77);f->calls++;
    if(f->io_error)return f->io_error;
    if(cmd==FH_ISP_STATS_REFRESH) {
        assert(n==4 && wire[0]==100 && !wire[1]&&!wire[2]&&!wire[3]);
        f->status_calls=f->offset_calls=0;return f->timeout?0:7;
    }
    if(cmd==FH_ISP_STATS_STATUS) {
        assert(n==16);put(wire,f->counter+(f->bad_status && f->status_calls++));return 0;
    }
    assert(cmd==FH_ISP_STATS_OFFSET && n==4);
    put(wire,f->offset+(f->bad_offset && f->offset_calls++?FH_ISP_STAT_BANK_BYTES:0));return 0;
}
static uint32_t *tile(struct fixture *f,unsigned bank,unsigned plane,unsigned index)
{return(uint32_t *)(uintptr_t)(f->memory.statistics.user+bank*FH_ISP_STAT_BANK_BYTES+0x5c8+plane*0x1000+index*16);}
static void fill(struct fixture *f,uint32_t mean)
{for(unsigned b=0;b<2;b++)for(unsigned p=0;p<4;p++)for(unsigned i=0;i<256;i++){
    uint32_t*t=tile(f,b,p,i);t[0]=mean*10;t[1]=10;t[2]=0xdeadbeef;t[3]=0xffffffff;
}}
static void setup(struct fixture *f)
{
    memset(f,0,sizeof*f);uint32_t need;assert(!fh_isp_memory_size(0,&need,&f->bytes));
    int flags=MAP_PRIVATE|MAP_ANONYMOUS;
#ifdef MAP_32BIT
    flags|=MAP_32BIT;
#endif
    f->ram=mmap(NULL,f->bytes,PROT_READ|PROT_WRITE,flags,-1,0);
    assert(f->ram!=MAP_FAILED && (uintptr_t)f->ram<=UINT32_MAX-f->bytes);
    assert(!fh_isp_memory_bind(0,FH_ISP_VMM_START,(uint32_t)(uintptr_t)f->ram,f->bytes,&f->memory));
    f->counter=10;f->grid=0x0f0f203b;
    struct fh_isp_stats_io io={f,lease,control,mmio};
    assert(!fh_isp_stats_init(&f->stats,&f->memory,77,&io));assert(!f->checks&&!f->calls);fill(f,1024);
}
static void done(struct fixture*f){assert(!munmap(f->ram,f->bytes));}
static void success(void)
{
    struct fixture f;setup(&f);struct fh_isp_stats_sample s;
    assert(!fh_isp_stats_read(&f.stats,100,&s));
    assert(s.generation==1&&s.count==10240&&s.sum==10485760);
    assert(s.raw_mean_q16==16383&&s.mean_q16==32767&&s.source_irq_counter==10&&!s.bank_offset);
    assert(s.shadow_valid&&s.shadow_width==1927&&s.shadow_height==1087);
    assert(s.shadow_min_log_q8<=s.shadow_max_log_q8&&s.shadow_max_log_q8==3584);
    assert(f.calls==5&&f.checks==8);
    assert(fh_isp_stats_read(&f.stats,100,&s)==-EAGAIN);
    /* Successful refresh + advanced source counter is fresh even when a
     * ring's physical bank index repeats after several elapsed frames. */
    f.counter+=4;assert(!fh_isp_stats_read(&f.stats,100,&s)&&s.generation==2&&!s.bank_offset);
    f.counter++;f.offset=FH_ISP_STAT_BANK_BYTES;
    assert(!fh_isp_stats_read(&f.stats,100,&s)&&s.generation==3);
    f.counter=UINT32_MAX;f.stats.previous_counter=UINT32_MAX-1;f.offset=0;
    assert(!fh_isp_stats_read(&f.stats,100,&s));
    f.counter=1;f.offset=FH_ISP_STAT_BANK_BYTES;assert(!fh_isp_stats_read(&f.stats,100,&s));
    fill(&f,4096);f.counter++;f.offset=0;
    assert(!fh_isp_stats_read(&f.stats,100,&s)&&s.raw_mean_q16==65535&&s.mean_q16==65535);
    done(&f);
}
static void rejection(void)
{
    for(unsigned mode=0;mode<9;mode++) {
        struct fixture f;setup(&f);struct fh_isp_stats_sample s,old;memset(&s,0xa5,sizeof s);old=s;
        int expected=-EAGAIN;
        if(mode==0)f.timeout=1;
        if(mode==1)f.bad_status=1;
        if(mode==2)f.bad_offset=1;
        if(mode==3){f.offset=1;expected=-ERANGE;}
        if(mode==4){f.grid=0;expected=-ENOTSUP;}
        if(mode==5){fill(&f,0);for(unsigned p=0;p<4;p++)for(unsigned i=0;i<256;i++)tile(&f,0,p,i)[1]=0;}
        if(mode==6){tile(&f,0,0,0)[1]=0;expected=-ERANGE;}
        if(mode==7){tile(&f,0,0,0)[0]=40961;expected=-ERANGE;}
        if(mode==8){f.io_error=-0x1234;expected=-0x1234;}
        assert(fh_isp_stats_read(&f.stats,100,&s)==expected);
        assert(!memcmp(&s,&old,sizeof s)&&!f.stats.generation);
        if(expected!=-EAGAIN)assert(fh_isp_stats_read(&f.stats,100,&s)==-EPIPE);
        done(&f);
    }
}
static void zeros_and_bounds(void)
{
    struct fixture f;setup(&f);struct fh_isp_stats_sample s;
    tile(&f,0,0,0)[0]=tile(&f,0,0,0)[1]=0;
    assert(!fh_isp_stats_read(&f.stats,100,&s)&&s.count==10230&&s.raw_mean_q16==16383);
    assert(fh_isp_stats_read(&f.stats,0,&s)==-EINVAL&&fh_isp_stats_read(&f.stats,101,&s)==-EINVAL);
    f.offset=FH_ISP_STAT_BANK_BYTES;f.counter++;f.stats.generation=UINT64_MAX;
    assert(fh_isp_stats_read(&f.stats,100,&s)==-EOVERFLOW);done(&f);
}
static void owner_failures(void)
{
    for(unsigned n=1;n<=8;n++) {
        struct fixture f;setup(&f);f.fail_at=n;struct fh_isp_stats_sample s;
        assert(fh_isp_stats_read(&f.stats,100,&s)==-EACCES&&f.stats.faulted&&!f.stats.generation);
        unsigned calls=f.calls;assert(fh_isp_stats_read(&f.stats,100,&s)==-EPIPE&&f.calls==calls);done(&f);
    }
}
static void colour_cells(void)
{
    struct fixture f;setup(&f);struct fh_isp_stats_sample s;
    uint32_t *rgb=(uint32_t *)(uintptr_t)(f.memory.statistics.user+0x48);
    for(unsigned i=0;i<9;i++){rgb[4*i]=1000+i;rgb[4*i+1]=2000+i;rgb[4*i+2]=500+i;rgb[4*i+3]=10;}
    assert(!fh_isp_stats_read(&f.stats,100,&s)&&s.awb_valid&&s.awb.generation==s.generation);
    assert(s.awb.cell[8].red==1008&&s.awb.cell[8].count==10);
    f.counter++;rgb[0]=40961;assert(!fh_isp_stats_read(&f.stats,100,&s)&&!s.awb_valid); /* AE stays alive. */
    uint32_t *hist=(uint32_t *)(uintptr_t)(f.memory.statistics.user+0x21250);
    hist[2]=70000;f.counter++;
    assert(!fh_isp_stats_read(&f.stats,100,&s)&&!s.shadow_valid&&s.raw_mean_q16==16383&&!f.stats.faulted);
    done(&f);
}
int main(void)
{colour_cells();success();rejection();zeros_and_bounds();owner_failures();puts("ISP stats:4groups passed; owned RAM and syscall fakes only");return 0;}
