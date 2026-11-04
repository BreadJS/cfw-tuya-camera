#include "fh_capture.h"
#include <errno.h>
#include <sys/ioctl.h>

int fh_capture_linux_ioctl(void *opaque,int fd,uint32_t cmd,uint8_t *wire,size_t bytes)
{
    size_t expected;
    int rc;
    (void)opaque;
    switch (cmd) {
    case FH_CAP_VPU_SYS_QUERY: case FH_CAP_PAE_SYS_QUERY:
    case FH_CAP_VPU_OPEN: case FH_CAP_VPU_ENABLE: case FH_CAP_PAE_START:
    case FH_CAP_PAE_IDR: case FH_CAP_PAE_STOP: case FH_CAP_PAE_RELEASE: expected=4; break;
    case FH_CAP_VPU_SYS_INIT: case FH_CAP_PAE_SYS_INIT: case FH_CAP_VPU_VI:
    case FH_CAP_VPU_CONFIG: case FH_CAP_VPU_GET_CAPACITY: expected=12; break;
    case FH_CAP_VPU_QUERY: expected=16; break;
    case FH_CAP_VPU_MEMORY: expected=24; break;
    case FH_CAP_VPU_PACE_SET: case FH_CAP_VPU_PACE_GET: case FH_CAP_MEDIA_BIND: expected=8; break;
    case FH_CAP_PAE_QUERY: expected=20; break;
    case FH_CAP_PAE_MEMORY: expected=28; break;
    case FH_CAP_PAE_CONFIG: expected=44; break;
    case FH_CAP_PAE_RC: expected=84; break;
    case FH_CAP_MEDIA_POLL: expected=368; break;
    default: return -ENOTSUP;
    }
    if (fd<0 || !wire || bytes!=expected) return -EINVAL;
    rc=ioctl(fd,(unsigned long)cmd,wire);
    /* These kernels also return vendor negative codes outside Linux's errno
     * range, so preserve rc unless libc reported the conventional -1. */
    return rc==-1 ? -errno : rc;
}
