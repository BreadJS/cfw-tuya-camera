#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "linux_io.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "high physical offsets need wide mmap offsets");
#define ISP_PHYSICAL UINT32_C(0xe8400000)
#define ISP_EXTENT UINT32_C(0x4000)
static int checked(int rc) { return rc > 0 ? -EIO : rc; }
static int owned(struct fh_linux_io *s)
{
    if (!s || !s->check || !s->quiesce) return -EINVAL;
    if (s->quarantined) return -ENOTRECOVERABLE;
    return checked(s->check(s->owner));
}
static int close_fd(struct fh_linux_io *s, int *fd)
{
    if (*fd < 0) return 0;
    int original = *fd;
    *fd = -1; /* Linux consumes close even on EINTR; never retry a reused fd. */
    if (!close(original)) return 0;
    int rc = -errno;
    s->quarantined = 1;
    return rc;
}
static int device(struct fh_linux_io *s, const char *path, unsigned maj,
                  unsigned min, int *fd)
{
    if (*fd >= 0) return 0;
    int sync_flags = maj == 1 && min == 1 ? O_SYNC : 0;
    *fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | sync_flags);
    if (*fd < 0) return -errno;
    struct stat st;
    int rc = fstat(*fd, &st) ? -errno :
        (!S_ISCHR(st.st_mode) || major(st.st_rdev) != maj || minor(st.st_rdev) != min) ? -ENODEV : 0;
    if (rc) {
        int close_rc = close_fd(s, fd);
        return close_rc ? close_rc : rc;
    }
    return 0;
}
int fh_linux_io_init(struct fh_linux_io *s, void *owner,
                     int (*check)(void *), int (*quiesce)(void *))
{
    if (!s || !check || !quiesce) return -EINVAL;
    *s = (struct fh_linux_io){.owner=owner, .check=check, .quiesce=quiesce,
                              .mem_fd=-1, .i2c_fd=-1};
    return 0;
}
static int map_region(struct fh_linux_io *s, struct fh_linux_mapping *m,
                      uint32_t physical, uint32_t extent)
{
    int rc = owned(s);
    if (rc) return rc;
    if (m->mapped) return -EALREADY;
    rc = device(s, "/dev/mem", 1, 1, &s->mem_fd);
    if (rc) return rc;
    /* Convert through uint64_t to avoid sign extension on ARM32. */
    void *address = mmap(NULL, extent, PROT_READ | PROT_WRITE, MAP_SHARED,
                         s->mem_fd, (off_t)(uint64_t)physical);
    if (address == MAP_FAILED) return -errno;
    *m = (struct fh_linux_mapping){address, extent, 1};
    return 0;
}
static int unmap_region(struct fh_linux_mapping *m)
{
    if (!m->mapped) return 0;
    if (munmap(m->address, m->extent)) return -errno;
    *m = (struct fh_linux_mapping){0};
    return 0;
}
static int mipi_map(void *opaque, enum mis2006_mipi_region region,
                     uint32_t physical, uint32_t extent)
{
    struct fh_linux_io *s = opaque;
    if (!s || (unsigned)region >= MIPI_REGION_COUNT ||
        physical != mis2006_mipi_mappings[region].physical ||
        extent != mis2006_mipi_mappings[region].length) return -EINVAL;
    return map_region(s, &s->mipi[region], physical, extent);
}
static int mipi_unmap(void *opaque, enum mis2006_mipi_region region)
{
    struct fh_linux_io *s = opaque;
    int rc = owned(s);
    if (rc) return rc;
    if ((unsigned)region >= MIPI_REGION_COUNT) return -EINVAL;
    /* CPU unmapping only: callers must not interpret this as stopping DMA. */
    return unmap_region(&s->mipi[region]);
}
static int read32(struct fh_linux_io *s, struct fh_linux_mapping *m,
                  uint32_t offset, uint32_t *out)
{
    int rc = owned(s);
    if (rc) return rc;
    if (!out || !m->mapped || (offset & 3) || m->extent < 4 || offset > m->extent - 4)
        return -EINVAL;
    __sync_synchronize();
    uint32_t value = *(volatile uint32_t *)((unsigned char *)m->address + offset);
    __sync_synchronize();
    *out = value;
    return 0;
}
static int write32(struct fh_linux_io *s, struct fh_linux_mapping *m,
                   uint32_t offset, uint32_t value)
{
    int rc = owned(s);
    if (rc) return rc;
    if (!m->mapped || (offset & 3) || m->extent < 4 || offset > m->extent - 4)
        return -EINVAL;
    s->touched = 1;
    __sync_synchronize();
    *(volatile uint32_t *)((unsigned char *)m->address + offset) = value;
    __sync_synchronize();
    return 0;
}
static int mipi_read(void *opaque, enum mis2006_mipi_region region, uint32_t offset, uint32_t *out)
{
    struct fh_linux_io *s = opaque;
    if (!s || (unsigned)region >= MIPI_REGION_COUNT) return -EINVAL;
    return read32(s, &s->mipi[region], offset, out);
}
static int mipi_write(void *opaque, enum mis2006_mipi_region region, uint32_t offset, uint32_t value)
{
    struct fh_linux_io *s = opaque;
    if (!s || (unsigned)region >= MIPI_REGION_COUNT) return -EINVAL;
    return write32(s, &s->mipi[region], offset, value);
}
int fh_linux_io_map_isp(struct fh_linux_io *s)
{
    if (!s) return -EINVAL;
    return map_region(s, &s->isp, ISP_PHYSICAL, ISP_EXTENT);
}
int fh_linux_io_isp_read(void *opaque, uint32_t offset, uint32_t *out)
{
    struct fh_linux_io *s = opaque;
    return s ? read32(s, &s->isp, offset, out) : -EINVAL;
}
int fh_linux_io_isp_write(void *opaque, uint32_t offset, uint32_t value)
{
    struct fh_linux_io *s = opaque;
    return s ? write32(s, &s->isp, offset, value) : -EINVAL;
}
int fh_linux_io_bus_open(void *opaque, uint8_t address)
{
    struct fh_linux_io *s = opaque;
    int rc = owned(s);
    if (rc) return rc;
    if (address != 0x30 && address != 0x31) return -EINVAL;
    if (s->i2c_fd >= 0) return -EBUSY;
    rc = device(s, "/dev/i2c-0", 89, 0, &s->i2c_fd);
    if (rc) return rc;
    /* Leave a failed setup fd tracked for the runtime's required cleanup. */
    int result = ioctl(s->i2c_fd, I2C_TENBIT, 0UL);
    if (result) return result < 0 ? -errno : -EIO;
    result = ioctl(s->i2c_fd, I2C_SLAVE_FORCE, (unsigned long)address);
    if (result) return result < 0 ? -errno : -EIO;
    s->i2c_address = address;
    return 0;
}
int fh_linux_io_bus_close(void *opaque)
{
    struct fh_linux_io *s = opaque;
    if (!s) return -EINVAL;
    int rc = close_fd(s, &s->i2c_fd);
    if (!rc) s->i2c_address = 0;
    return rc;
}
int fh_linux_io_bus_write(void *opaque, const uint8_t wire[3])
{
    struct fh_linux_io *s = opaque;
    int rc = owned(s);
    if (rc) return rc;
    if (!wire || s->i2c_fd < 0 || !s->i2c_address) return -EINVAL;
    struct i2c_msg message = {.addr=s->i2c_address, .len=3, .buf=(uint8_t *)wire};
    struct i2c_rdwr_ioctl_data request = {.msgs=&message, .nmsgs=1};
    s->touched = 1;
    int result = ioctl(s->i2c_fd, I2C_RDWR, &request);
    return result < 0 ? -errno : result == 1 ? 0 : -EREMOTEIO;
}
int fh_linux_io_bus_read(void *opaque, const uint8_t address[2], uint8_t *out)
{
    struct fh_linux_io *s = opaque;
    int rc = owned(s);
    if (rc) return rc;
    if (!address || !out || s->i2c_fd < 0 || !s->i2c_address) return -EINVAL;
    uint8_t value;
    struct i2c_msg messages[2] = {
        {.addr=s->i2c_address, .len=2, .buf=(uint8_t *)address},
        {.addr=s->i2c_address, .flags=I2C_M_RD, .len=1, .buf=&value}
    };
    struct i2c_rdwr_ioctl_data request = {.msgs=messages, .nmsgs=2};
    s->touched = 1; /* Address-write may occur even if the following read fails. */
    int result = ioctl(s->i2c_fd, I2C_RDWR, &request);
    if (result < 0) return -errno;
    if (result != 2) return -EREMOTEIO;
    *out = value;
    return 0;
}
int fh_linux_io_delay(void *opaque, uint32_t us)
{
    int rc = owned(opaque);
    if (rc) return rc;
    if (us > 10000000) return -ERANGE;
    struct timespec time = {.tv_sec=us/1000000, .tv_nsec=(long)(us%1000000)*1000};
    while (nanosleep(&time, &time)) if (errno != EINTR) return -errno;
    return 0;
}
int fh_linux_io_mipi_ops(struct fh_linux_io *s, struct mis2006_mipi_io *out)
{
    if (!s || !s->check || !s->quiesce || !out) return -EINVAL;
    *out = (struct mis2006_mipi_io){s,mipi_map,mipi_unmap,mipi_read,mipi_write,fh_linux_io_delay};
    return 0;
}
int fh_linux_io_fixed_ops(struct fh_linux_io *s, struct fh_isp_fixed_io *out)
{
    if (!s || !s->check || !s->quiesce || !out) return -EINVAL;
    *out = (struct fh_isp_fixed_io){s,fh_linux_io_isp_read,fh_linux_io_isp_write};
    return 0;
}
int fh_linux_io_geometry_ops(struct fh_linux_io *s, struct fh_isp_geometry_io *out)
{
    if (!s || !s->check || !s->quiesce || !out) return -EINVAL;
    *out = (struct fh_isp_geometry_io){s,fh_linux_io_isp_read,fh_linux_io_isp_write};
    return 0;
}
int fh_linux_io_close(struct fh_linux_io *s)
{
    int rc = owned(s);
    if (rc) return rc;
    if (s->touched) {
        rc = checked(s->quiesce(s->owner));
        if (rc) return rc;
    }
    rc = fh_linux_io_bus_close(s);
    if (rc) return rc;
    for (unsigned i = 0; i < MIPI_REGION_COUNT; i++) {
        rc = unmap_region(&s->mipi[i]);
        if (rc) return rc;
    }
    rc = unmap_region(&s->isp);
    if (rc) return rc;
    rc = close_fd(s, &s->mem_fd);
    if (!rc) s->touched = 0;
    return rc;
}
