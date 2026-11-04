#include "control.h"
#include "day_gamma.h"
#include "presets.h"
#include <errno.h>
#include <string.h>
_Static_assert(sizeof(camera_day_gamma)==652,"Matching ISP driver copies 0x28c bytes");
int camera_gamma_submit(struct fh_linux_io *platform,int fd,const struct fh_isp_control_io *io)
{return camera_gamma_submit_profile(platform,fd,io,5,1);}
int camera_gamma_submit_profile(struct fh_linux_io *platform,int fd,const struct fh_isp_control_io *io,unsigned profile,unsigned enabled)
{
    if(!platform||!platform->check||fd<0||!io||!io->ioctl||profile>9||enabled>1)return -EINVAL;
    int rc=platform->check(platform->owner);if(rc)return rc<0?rc:-EPERM;
    uint32_t route;
    rc=fh_linux_io_isp_read(platform,0x4f0,&route);if(rc)return rc;
    uint32_t wire[CAMERA_GAMMA_WORDS];memcpy(wire,camera_day_gamma,sizeof wire);
    memcpy(wire+80,camera_gamma_presets[profile],sizeof camera_gamma_presets[0]);
    wire[160]=wire[161]=enabled;
    rc=io->ioctl(io->opaque,fd,CAMERA_GAMMA_SET,(uint8_t *)wire,sizeof wire);
    if(rc)return rc<0?rc:-EPROTO;
    /* Same masked mode1 route change as original110710. Preserve all other
     * fields, including those configured by unrelated processing stages. */
    return fh_linux_io_isp_write(platform,0x4f0,route&~UINT32_C(0x10));
}
int camera_gamma_verify(struct fh_linux_io *platform)
{return camera_gamma_verify_enabled(platform,1);}
int camera_gamma_verify_enabled(struct fh_linux_io *platform,unsigned enabled)
{
    if(!platform||enabled>1)return -EINVAL;
    uint32_t enable,route;int rc=fh_linux_io_isp_read(platform,0x28,&enable);
    if(!rc)rc=fh_linux_io_isp_read(platform,0x4f0,&route);
    if(rc)return rc;
    return (enable&0x410)!=(enabled?0u:0x410u)||(route&0x10)?-EAGAIN:0;
}
