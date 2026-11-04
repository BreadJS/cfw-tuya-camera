#define _GNU_SOURCE
#include "fh_capacity.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
int fh_capacity_posix_init(struct fh_capacity_posix *p,void *opaque,int (*verify)(void *))
{
    if (!p || !verify) return -ENOTSUP;
    memset(p,0,sizeof(*p));p->lease_opaque=opaque;p->verify_lease=verify;p->retained_fd=-1;
    return 0;
}
int fh_capacity_posix_verify(void *opaque)
{
    struct fh_capacity_posix *p=opaque;
    if (!p || !p->verify_lease) return -ENOTSUP;
    if (p->quarantine) return -ENOTRECOVERABLE;
    return p->verify_lease(p->lease_opaque);
}
ptrdiff_t fh_capacity_posix_write(void *opaque,const char *command,size_t bytes)
{
    struct fh_capacity_posix *p=opaque;
    ssize_t result;
    int fd,rc,primary=0;
    static const char expected[]="cap_0_1920_1080";
    rc=fh_capacity_posix_verify(p);
    if (rc) return rc>0 ? -EIO : rc;
    if (!command || bytes!=sizeof(expected)-1 || memcmp(command,expected,bytes)) return -EINVAL;
    fd=open("/proc/driver/vpu",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) { p->last_error=-errno;return p->last_error; }
    p->retained_fd=fd;
    /* One complete stock command, no newline/NUL, no short-write suffix retry. */
    result=write(fd,command,bytes);
    if (result<0) primary=-errno;
    else if ((size_t)result!=bytes) primary=-EIO;
    if (close(fd)<0) {
        p->last_close_error=-errno;p->quarantine=1;
        /* Linux consumed non-EBADF fd; keep number only as diagnostic, never
         * close it again. The capacity object will fault and require recovery. */
        p->last_error=primary ? primary : p->last_close_error;
        return p->last_error;
    }
    p->retained_fd=-1;
    if (primary) { p->last_error=primary;return primary; }
    return result;
}
