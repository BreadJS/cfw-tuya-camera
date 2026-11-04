#define _POSIX_C_SOURCE 200809L
#include "fh_audio.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int linux_open(void *p, const char *path, int flags)
{ (void)p; int rc = open(path, flags); return rc < 0 ? -errno : rc; }
static int linux_close(void *p, int fd)
{ (void)p; return close(fd) ? -errno : 0; }
static int linux_stat(void *p, int fd, struct stat *st)
{ (void)p; return fstat(fd, st) ? -errno : 0; }
static int linux_lstat(void *p, const char *path, struct stat *st)
{ (void)p; return lstat(path, st) ? -errno : 0; }
static int linux_ioctl(void *p, int fd, uint32_t command, uint8_t *wire, size_t size)
{
    (void)p;
    if ((command == 0 && (!wire || size != 32)) ||
        (command == 23 && (!wire || size != 16)) ||
        ((command == 5 || command == 7) && (wire || size)) ||
        (command != 0 && command != 5 && command != 7 && command != 23)) return -EINVAL;
    /* Positive/vendor negative returns are preserved; only libc -1 uses errno. */
    int rc = ioctl(fd, (unsigned long)command, wire);
    return rc == -1 ? -errno : rc;
}
static ssize_t linux_read(void *p, int fd, void *out, size_t size)
{
    (void)p;
    if (!out || !size || size > FH_AUDIO_MAX_BYTES) return -EINVAL;
    ssize_t rc = read(fd, out, size);
    return rc == -1 ? -errno : rc;
}
static int linux_pointer(void *p, void *pointer, uint32_t *out)
{
    (void)p;
    uintptr_t address = (uintptr_t)pointer;
    if (sizeof(void *) != 4 || address > UINT32_MAX || !address) return -ENOTSUP;
    *out = (uint32_t)address;
    return 0;
}
const struct fh_audio_io *fh_audio_linux_io(void)
{
    static const struct fh_audio_io io = {
        NULL, linux_open, linux_close, linux_stat, linux_lstat, linux_ioctl, linux_read, linux_pointer
    };
    return &io;
}
