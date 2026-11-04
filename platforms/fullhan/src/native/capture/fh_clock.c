#define _POSIX_C_SOURCE 200809L
#include "fh_clock.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int fail(struct fh_clock *s,int rc)
{ s->calibrated=0;s->faulted=1;s->last_error=rc>0?-EIO:rc;return s->last_error; }
static int owner(struct fh_clock *s)
{
    if(!s || !s->io.check || s->pae_fd<0)return -EINVAL;
    int rc=s->io.check(s->io.opaque);return rc>0?-EIO:rc;
}
int fh_clock_init(struct fh_clock *s,int fd,const struct fh_clock_io *io)
{
    if(!s || fd<0 || !io || !io->check || !io->ioctl || !io->monotonic_ns)return -EINVAL;
    *s=(struct fh_clock){.io=*io,.pae_fd=fd};return 0;
}
int fh_clock_calibrate(struct fh_clock *s,unsigned samples)
{
    if(!s || !s->io.ioctl || !s->io.monotonic_ns || samples<2 || samples>FH_CLOCK_MAX_SAMPLES)return -EINVAL;
    if(s->attempted || s->faulted || s->calibrated)return -EALREADY;
    s->attempted=1;
    int rc=owner(s);if(rc)return fail(s,rc);
    uint8_t rate[4]={0};
    rc=s->io.ioctl(s->io.opaque,s->pae_fd,FH_CLOCK_RATE,rate,sizeof rate);
    if(rc)return fail(s,rc);
    s->rate=le32(rate);
    if(s->rate!=FH_CLOCK_REQUIRED_RATE)return fail(s,-ENOTSUP);
    uint64_t previous_raw=0,previous_after=0,best=UINT64_MAX;
    unsigned advanced=0;
    for(unsigned i=0;i<samples;i++) {
        uint64_t before,after;uint8_t current[8]={0};
        rc=owner(s);if(rc)return fail(s,rc);
        rc=s->io.monotonic_ns(s->io.opaque,&before);if(rc)return fail(s,rc);
        rc=s->io.ioctl(s->io.opaque,s->pae_fd,FH_CLOCK_CURRENT,current,sizeof current);
        if(rc)return fail(s,rc);
        rc=s->io.monotonic_ns(s->io.opaque,&after);if(rc)return fail(s,rc);
        rc=owner(s);if(rc)return fail(s,rc);
        uint64_t raw=(uint64_t)le32(current)|(uint64_t)le32(current+4)<<32;
        if(after<=before || (i && before<previous_after) || (i && raw<previous_raw))return fail(s,-ERANGE);
        if(i && raw>previous_raw)advanced=1;
        uint64_t width=after-before;
        if(width<best) {
            best=width;s->raw_reference_us=raw;
            s->monotonic_reference_ns=before+width/2;
        }
        previous_raw=raw;previous_after=after;s->samples=i+1;
    }
    if(!advanced)return fail(s,-ERANGE);
    if(best>=FH_CLOCK_MAX_WINDOW_NS)return fail(s,-ETIMEDOUT);
    s->window_ns=best;s->calibrated=1;return 0;
}
int fh_clock_raw_to_ns(void *opaque,uint64_t raw,uint64_t *out)
{
    struct fh_clock *s=opaque;
    if(!s || !out)return -EINVAL;
    if(!s->calibrated || s->faulted)return -EPIPE;
    int rc=owner(s);if(rc)return fail(s,rc);
    uint64_t reference=s->raw_reference_us,anchor=s->monotonic_reference_ns,value,delta;
    if(raw>=reference) {
        delta=raw-reference;
        if(delta>(UINT64_MAX-anchor)/1000)return fail(s,-ERANGE);
        value=anchor+delta*1000;
    } else {
        delta=reference-raw;
        if(delta>anchor/1000)return fail(s,-ERANGE);
        value=anchor-delta*1000;
    }
    *out=value;return 0;
}
int fh_clock_frame_clock(struct fh_clock *s,struct fh_frame_clock *out)
{
    if(!s || !out)return -EINVAL;
    if(!s->calibrated || s->faulted)return -EPIPE;
    int rc=owner(s);if(rc)return fail(s,rc);
    *out=(struct fh_frame_clock){s,fh_clock_raw_to_ns};return 0;
}
int fh_clock_linux_ioctl(void *opaque,int fd,uint32_t command,uint8_t *wire,size_t bytes)
{
    (void)opaque;
    if(command!=FH_CLOCK_CURRENT && command!=FH_CLOCK_RATE)return -ENOTSUP;
    size_t expected=command==FH_CLOCK_CURRENT?8:4;
    if(fd<0 || !wire || bytes!=expected)return -EINVAL;
    int rc=ioctl(fd,(unsigned long)command,wire);
    return rc==-1?-errno:rc;
}
int fh_clock_monotonic_ns(void *opaque,uint64_t *out)
{
    (void)opaque;if(!out)return -EINVAL;
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return -errno;
    if(t.tv_sec<0 || t.tv_nsec<0 || t.tv_nsec>=1000000000)return -ERANGE;
    uint64_t seconds=(uint64_t)t.tv_sec,nanoseconds=(uint64_t)t.tv_nsec;
    if(seconds>(UINT64_MAX-nanoseconds)/UINT64_C(1000000000))return -ERANGE;
    *out=seconds*UINT64_C(1000000000)+nanoseconds;return 0;
}
