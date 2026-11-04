#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "isp_color_seed.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(struct fh_isp_color_seed)==FH_ISP_COLOR_SEED_BYTES,"color seed record");
static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int valid(const struct fh_isp_color_seed *s)
{
    if(!s || s->magic!=FH_ISP_COLOR_SEED_MAGIC || (s->version!=1 && s->version!=2) || s->width!=1920 || s->height!=1080)return -EPROTO;
    uint32_t gains[2]={s->gain01,s->gain23};
    for(unsigned i=0;i<2;i++)for(unsigned shift=0;shift<=16;shift+=16) {
        uint32_t gain=(gains[i]>>shift)&0xffff;
        if(!gain || gain>0xfff)return -ERANGE;
    }
    if(s->clip>0x3ff)return -ERANGE;
    if(s->version==2) {
        for(unsigned i=0;i<2;i++)if(s->blc[i]&UINT32_C(0xf000f000))return -ERANGE;
        for(unsigned i=0;i<6;i++)if(s->ccm[i]&UINT32_C(0xe000e000))return -ERANGE;
    }
    return 0;
}
int fh_isp_color_seed_decode(const uint8_t *wire,size_t bytes,struct fh_isp_color_seed *out)
{
    if(!wire || !out || (bytes!=FH_ISP_COLOR_SEED_V1_BYTES && bytes!=FH_ISP_COLOR_SEED_BYTES))return -EINVAL;
    struct fh_isp_color_seed seed={.magic=get(wire),.version=get(wire+4),
        .width=get(wire+8),.height=get(wire+12),.reg24=get(wire+16),
        .gain01=get(wire+20),.gain23=get(wire+24),.clip=get(wire+28)};
    if((seed.version==1 && bytes!=32) || (seed.version==2 && bytes!=92))return -EPROTO;
    if(seed.version==2) {
        for(unsigned i=0;i<2;i++)seed.blc[i]=get(wire+32+4*i);
        for(unsigned i=0;i<6;i++)seed.ccm[i]=get(wire+40+4*i);
        for(unsigned i=0;i<7;i++)seed.yc[i]=get(wire+64+4*i);
    }
    int rc=valid(&seed);if(rc)return rc;
    *out=seed;return 0;
}
static int exact(int fd,uint8_t *wire,size_t bytes)
{
    for(size_t done=0;done<bytes;) {
        ssize_t n=pread(fd,wire+done,bytes-done,(off_t)done);
        if(n<0 && errno==EINTR)continue;
        if(n<0)return -errno;
        if(!n)return -EPROTO;
        done+=(size_t)n;
    }
    return 0;
}
int fh_isp_color_seed_read(const char *path,struct fh_isp_color_seed *out)
{
    if(!path || !out)return -EINVAL;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    struct stat before,after;uint8_t wire[FH_ISP_COLOR_SEED_BYTES],again[FH_ISP_COLOR_SEED_BYTES];size_t bytes=0;int rc=0;
    if(fstat(fd,&before))rc=-errno;
    else if(!S_ISREG(before.st_mode) || before.st_uid!=0 || (before.st_mode&022) || (before.st_size!=32 && before.st_size!=92))rc=-EPERM;
    if(!rc)bytes=(size_t)before.st_size;
    if(!rc)rc=exact(fd,wire,bytes);
    if(!rc)rc=exact(fd,again,bytes);
    if(!rc && fstat(fd,&after))rc=-errno;
    if(!rc && (memcmp(wire,again,bytes) || after.st_size!=before.st_size || after.st_uid!=0 || (after.st_mode&022) ||
        before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec!=after.st_ctim.tv_sec || before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec))rc=-ESTALE;
    if(close(fd) && !rc)rc=-errno;
    return rc?rc:fh_isp_color_seed_decode(wire,bytes,out);
}
static int fault(struct fh_isp_color_seed_state *s,int rc)
{ s->faulted=1;s->last_error=rc>0?-EIO:rc;return s->last_error; }
int fh_isp_color_seed_apply(const struct fh_isp_color_seed *seed,struct fh_linux_io *io,
                            struct fh_isp_color_seed_state *state)
{
    if(!state || !io || !io->check || !io->quiesce || !io->isp.mapped || !io->isp.address ||
       ((uintptr_t)io->isp.address&3) || io->isp.extent<0x4c0 ||
       (seed && seed->version==2 && io->isp.extent<0x5d4))return -EINVAL;
    if(state->faulted)return -EPIPE;
    if(state->attempted)return -EALREADY;
    int rc=valid(seed);if(rc)return rc;
    state->attempted=1;
    rc=fh_linux_io_isp_read(io,0x24,&state->previous24);if(rc)return fault(state,rc);
    if((state->previous24^seed->reg24)&0x1e)return fault(state,-EINVAL);
    state->applied24=(seed->reg24&~UINT32_C(0x800000))|(state->previous24&UINT32_C(0x800000));
    uint32_t offsets[19]={0x224,0x228,0x4bc};
    uint32_t values[19]={seed->gain01,seed->gain23,seed->clip};
    unsigned count=3;
    if(seed->version==2) {
        for(unsigned i=0;i<2;i++) {offsets[count]=0x84+4*i;values[count++]=seed->blc[i];}
        for(unsigned i=0;i<6;i++) {offsets[count]=0x4c0+4*i;values[count++]=seed->ccm[i];}
        const uint32_t yc_offsets[7]={0x4dc,0x4e0,0x4e4,0x4e8,0x5c8,0x5cc,0x5d0};
        for(unsigned i=0;i<7;i++) {offsets[count]=yc_offsets[i];values[count++]=seed->yc[i];}
    }
    offsets[count]=0x24;values[count++]=state->applied24;
    for(unsigned i=0;i<count;i++) {
        rc=fh_linux_io_isp_write(io,offsets[i],values[i]);if(rc)return fault(state,rc);
        state->writes_completed=i+1;
    }
    state->applied=1;return 0;
}
