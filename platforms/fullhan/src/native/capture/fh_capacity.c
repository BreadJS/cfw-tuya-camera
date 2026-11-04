#include "fh_capacity.h"
#include "fh_capture.h"
#include <errno.h>
#include <string.h>
int fh_capacity_init(struct fh_capacity *c,const struct fh_capacity_io *io)
{
    if (!c || !io) return -EINVAL;
    if (!io->verify_lease || !io->write) return -ENOTSUP;
    memset(c,0,sizeof(*c));c->io=*io;return 0;
}
int fh_capacity_prepare(struct fh_capacity *c)
{
    static const char command[]="cap_0_1920_1080";
    ptrdiff_t result;
    int rc;
    if (!c || !c->io.verify_lease || !c->io.write) return -EINVAL;
    if (c->attempted || c->fault) return -EALREADY;
    rc=c->io.verify_lease(c->io.opaque);
    if (rc) return rc>0 ? -EIO : rc;
    c->attempted=1;
    result=c->io.write(c->io.opaque,command,sizeof(command)-1);
    if (result!=(ptrdiff_t)(sizeof(command)-1)) {
        c->fault=1;return result<0 ? (int)result : -EIO;
    }
    c->written=1;return 0;
}
int fh_capture_verify_capacity(struct fh_capture *s,struct fh_capacity *c)
{
    uint8_t w[12]={0};
    uint32_t width,height;
    int rc;
    if (!s || !c || !s->vmm || !s->vmm->owned || s->vmm->fault || !c->written ||
        c->fault || s->fault || s->last_completed!=FH_CAP_NONE) return -EPERM;
    rc=c->io.verify_lease(c->io.opaque);
    if (rc) return rc>0 ? -EIO : rc;
    rc=s->io.ioctl(s->io.opaque,s->isp_fd,FH_CAP_VPU_GET_CAPACITY,w,sizeof(w));
    s->last_command=FH_CAP_VPU_GET_CAPACITY;s->last_ioctl_result=rc;
    if (rc) goto fail;
    if (w[0] || w[1] || w[2] || w[3]) { rc=-EPROTO;goto fail; }
    width=(uint32_t)w[4]|(uint32_t)w[5]<<8|(uint32_t)w[6]<<16|(uint32_t)w[7]<<24;
    height=(uint32_t)w[8]|(uint32_t)w[9]<<8|(uint32_t)w[10]<<16|(uint32_t)w[11]<<24;
    if (width<1920 || height<1080) { rc=-ERANGE;goto fail; }
    c->actual_width=width;c->actual_height=height;c->readback=1;s->capacity_verified=1;
    return 0;
fail:
    c->fault=s->fault=1;s->last_error=rc>0 ? -EIO : rc;
    s->uncertain_stage=FH_CAP_CAPACITY_STAGE;return s->last_error;
}
