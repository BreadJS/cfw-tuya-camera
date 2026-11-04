#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "legacy_vmm_posix.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __linux__
#error "legacy VMM backend requires Linux close/mmap semantics"
#endif
/* Ensures 0xA2400000 is a positive file offset on ARM, not sign-extended from
 * a 32-bit signed off_t. Musl is 64-bit by default; glibc redirects mmap64. */
typedef char legacy_vmm_requires_64bit_off_t[(sizeof(off_t)>=8) ? 1 : -1];

static int failure(struct legacy_vmm_posix *b)
{
    int rc=-errno;
    b->last_syscall_error=rc;
    return rc;
}
static int valid_fd(struct legacy_vmm_posix *b, int fd)
{ return b && b->lease_held && fd>=0 && fd==b->fd; }
static int acquire(void *opaque)
{
    struct legacy_vmm_posix *b=opaque;
    int rc;
    if (!b || !b->lease.acquire) return -ENOTSUP;
    if (b->lease_held || b->fd>=0) return -EBUSY;
    rc=b->lease.acquire(b->lease.opaque);
    if (rc) return rc>0 ? -EIO : rc;
    b->lease_held=1;
    return 0;
}
static int release(void *opaque)
{
    struct legacy_vmm_posix *b=opaque;
    int rc;
    if (!b || !b->lease.release) return -ENOTSUP;
    if (!b->lease_held) return -EPERM;
    if (b->fd>=0) return -EBUSY;
    rc=b->lease.release(b->lease.opaque);
    if (rc) return rc>0 ? -EIO : rc;
    b->lease_held=0;
    return 0;
}
static int open_device(void *opaque, int *out)
{
    struct legacy_vmm_posix *b=opaque;
    int fd;
    if (!b || !out) return -EINVAL;
    if (!b->lease_held) return -EPERM;
    if (b->fd>=0) return -EBUSY;
    fd=open("/dev/vmm_userdev",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return failure(b);
    b->fd=fd;
    *out=fd;
    return 0;
}
static int close_device(void *opaque, int fd, int *consumed)
{
    struct legacy_vmm_posix *b=opaque;
    int rc, error;
    if (!consumed) return -EINVAL;
    *consumed=0;
    if (!valid_fd(b,fd)) return -EBADF;
    rc=close(fd);
    if (!rc) {
        b->fd=-1;
        *consumed=1;
        return 0;
    }
    error=errno;
    b->last_syscall_error=-error;
    /* Linux removes a valid fd from the table before filp_close, including on
     * EINTR/EIO. Do not retry it: the numeric fd may already have been reused.
     * EBADF means no close occurred and prior kernel release is unknown. In
     * either case adapter quarantine retains allocation records and lease. */
    b->fd=-1;
    *consumed=error==EBADF ? -1 : 1;
    return -error;
}
static int ioctl_request(void *opaque, int fd, uint32_t cmd, uint8_t wire[76])
{
    struct legacy_vmm_posix *b=opaque;
    int rc;
    if (!valid_fd(b,fd)) return -EBADF;
    if (!wire || (cmd!=LEGACY_VMM_ALLOC && cmd!=LEGACY_VMM_REMAP)) return -EINVAL;
    /* Only exact 76-byte legacy requests; never reset the zone via nr12.
     * Do not retry EINTR: an allocation or VMA may already have been created. */
    rc=ioctl(fd,(unsigned long)cmd,wire);
    return rc<0 ? failure(b) : rc;
}
static int map_allocation(void *opaque, int fd, uint32_t phys, uint32_t size,
                           unsigned prot, unsigned flags, uint32_t *out)
{
    struct legacy_vmm_posix *b=opaque;
    void *mapping;
    int native_flags=MAP_SHARED;
    if (!valid_fd(b,fd)) return -EBADF;
    if (!out || prot!=3 || flags!=1 || !size ||
        (size&(LEGACY_VMM_PAGE-1u)) || (phys&(LEGACY_VMM_PAGE-1u)) ||
        phys<LEGACY_VMM_ZONE_START || phys>=LEGACY_VMM_ZONE_END ||
        size>LEGACY_VMM_ZONE_END-phys) return -EINVAL;
#if UINTPTR_MAX > UINT32_MAX
#ifdef MAP_32BIT
    /* Host x86-64 verification still needs a representable legacy userspace
     * address. MAP_32BIT restricts the result; it is never used on ARMv6. */
    native_flags|=MAP_32BIT;
#else
    /* Do not create an unrepresentable VMA and then pretend cleanup succeeded.
     * Native integration targets 32-bit ARM; other 64-bit hosts are unsupported. */
    return -ENOTSUP;
#endif
#endif
    mapping=mmap(NULL,(size_t)size,PROT_READ|PROT_WRITE,native_flags,fd,
                 (off_t)(uint64_t)phys);
    if (mapping==MAP_FAILED) return failure(b);
    /* 32-bit ARM or MAP_32BIT makes this conversion lossless. No post-mapping
     * cleanup can fail before the allocation owner records the returned VMA. */
    *out=(uint32_t)(uintptr_t)mapping;
    return 0;
}
static int unmap_allocation(void *opaque, uint32_t user, uint32_t size)
{
    struct legacy_vmm_posix *b=opaque;
    if (!b || !b->lease_held || b->fd<0) return -EPERM;
    if (!size || (size&(LEGACY_VMM_PAGE-1u)) ||
        (user&(LEGACY_VMM_PAGE-1u)) || user>UINT32_MAX-size) return -EINVAL;
    /* Caller owns one complete VMA and excludes concurrent splitting/unmapping.
     * Linux validates/splits before unmapping; failure retains its pages. */
    if (munmap((void *)(uintptr_t)user,(size_t)size)<0) return failure(b);
    return 0;
}
int legacy_vmm_posix_init(struct legacy_vmm_posix *b,
                          const struct legacy_vmm_zone_lease *lease)
{
    if (!b || !lease) return -EINVAL;
    if (!lease->acquire || !lease->release) return -ENOTSUP;
    memset(b,0,sizeof(*b));
    b->lease=*lease;
    b->fd=-1;
    return 0;
}
const struct legacy_vmm_ops *legacy_vmm_posix_ops(void)
{
    static const struct legacy_vmm_ops ops={acquire,release,open_device,close_device,
        ioctl_request,map_allocation,unmap_allocation};
    return &ops;
}
