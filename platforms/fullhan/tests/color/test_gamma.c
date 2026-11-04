#include "../../src/native/isp/gamma/control.h"
#include "../../src/native/isp/gamma/day_gamma.h"
#include "../../src/native/isp/gamma/presets.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static int denied,io_error,writes,submits;
static unsigned expected_profile=5,expected_enabled=1;
static uint32_t route=0xabcdef31,enabled=0x2610;
static int owner(void *p){(void)p;return denied;}
int fh_linux_io_isp_read(void *p,uint32_t off,uint32_t *out)
{(void)p;if(denied)return denied;assert(off==0x28||off==0x4f0);*out=off==0x28?enabled:route;return 0;}
int fh_linux_io_isp_write(void *p,uint32_t off,uint32_t value)
{(void)p;assert(off==0x4f0);writes++;route=value;return 0;}
static int submit(void *p,int fd,uint32_t cmd,uint8_t *data,size_t n)
{
    (void)p;assert(fd==7&&cmd==0x40046924&&n==652);submits++;
    assert(!memcmp(data,camera_day_gamma,80*4));
    assert(!memcmp(data+80*4,camera_gamma_presets[expected_profile],80*4));
    const uint32_t *w=(const uint32_t *)data;
    assert(w[160]==expected_enabled&&w[161]==expected_enabled&&w[162]==1);
    /* Emulate delayed IRQ: submission alone does not activate gamma. */
    return io_error;
}
int main(void)
{
    struct fh_linux_io platform={.check=owner};struct fh_isp_control_io io={NULL,submit};
    assert(camera_gamma_submit(NULL,7,&io)==-EINVAL);
    assert(camera_gamma_submit(&platform,-1,&io)==-EINVAL);
    denied=-EPERM;assert(camera_gamma_submit(&platform,7,&io)==-EPERM);
    assert(!submits&&!writes);denied=0;io_error=-EIO;
    assert(camera_gamma_submit(&platform,7,&io)==-EIO);assert(!writes&&route==0xabcdef31);
    io_error=1;assert(camera_gamma_submit(&platform,7,&io)==-EPROTO);assert(!writes);
    io_error=0;assert(!camera_gamma_submit(&platform,7,&io));assert(writes==1&&route==0xabcdef21);
    assert(camera_gamma_verify(&platform)==-EAGAIN);
    enabled=0x2200;assert(!camera_gamma_verify(&platform));
    route|=0x10;assert(camera_gamma_verify(&platform)==-EAGAIN);
    denied=-EPERM;assert(camera_gamma_verify(&platform)==-EPERM);
    denied=0;
    int before=submits;
    assert(camera_gamma_submit_profile(&platform,7,&io,10,1)==-EINVAL&&submits==before);
    assert(camera_gamma_submit_profile(&platform,7,&io,5,2)==-EINVAL&&submits==before);
    assert(camera_gamma_verify_enabled(&platform,2)==-EINVAL);
    for(expected_profile=0;expected_profile<10;expected_profile++)for(expected_enabled=0;expected_enabled<=1;expected_enabled++){
        route=0xabcdef31;enabled=expected_enabled?0x2610:0x2200;
        assert(!camera_gamma_submit_profile(&platform,7,&io,expected_profile,expected_enabled));
        assert(route==0xabcdef21&&camera_gamma_verify_enabled(&platform,expected_enabled)==-EAGAIN);
        enabled=(enabled&~0x410u)|(expected_enabled?0:0x410u);
        assert(!camera_gamma_verify_enabled(&platform,expected_enabled));
        /* A single-stage mismatch is not accepted as an applied preset. */
        enabled^=0x400;assert(camera_gamma_verify_enabled(&platform,expected_enabled)==-EAGAIN);
    }
    return 0;
}
