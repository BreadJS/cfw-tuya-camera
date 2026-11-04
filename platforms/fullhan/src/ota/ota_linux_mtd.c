/* Real Linux NOR MTD backend. No main(), deployment or physical-apply caller. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "ota_linux_mtd.h"
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* Existing toolchain UAPI has obsolete field names at offsets 24..31;
 * Linux 4.9 calls these padding. None is interpreted. Assert wire ABI. */
_Static_assert(sizeof(struct mtd_info_user) == 32, "MTD info ABI");
_Static_assert(offsetof(struct mtd_info_user, type) == 0, "MTD type ABI");
_Static_assert(offsetof(struct mtd_info_user, flags) == 4, "MTD flags ABI");
_Static_assert(offsetof(struct mtd_info_user, size) == 8, "MTD size ABI");
_Static_assert(offsetof(struct mtd_info_user, erasesize) == 12, "MTD erase ABI");
_Static_assert(offsetof(struct mtd_info_user, writesize) == 16, "MTD write ABI");
_Static_assert(offsetof(struct mtd_info_user, oobsize) == 20, "MTD OOB ABI");
_Static_assert(sizeof(struct erase_info_user) == 8, "MTD erase request ABI");
_Static_assert(MEMGETINFO == 0x80204d01UL, "MEMGETINFO ABI");
_Static_assert(MEMERASE == 0x40084d02UL, "MEMERASE ABI");
_Static_assert(MEMGETREGIONCOUNT == 0x80044d07UL, "region count ABI");
_Static_assert(sizeof(off_t) == 8, "64-bit positioned I/O required");

static const char *const paths[2] = { "/dev/mtd3", "/dev/mtd4" };
static int real_open(void *c, const char *p, int flags) { (void)c; return open(p, flags); }
static int real_close(void *c, int fd) { (void)c; return close(fd); }
static int real_stat(void *c, int fd, struct stat *st) { (void)c; return fstat(fd, st); }
static int real_lstat(void *c, const char *p, struct stat *st) { (void)c; return lstat(p, st); }
static ssize_t real_read(void *c, int fd, void *p, size_t n) { (void)c; return read(fd, p, n); }
static ssize_t real_pread(void *c, int fd, void *p, size_t n, off_t off) { (void)c; return pread(fd, p, n, off); }
static ssize_t real_pwrite(void *c, int fd, const void *p, size_t n, off_t off) { (void)c; return pwrite(fd, p, n, off); }
static int real_ioctl(void *c, int fd, unsigned long req, void *p) { (void)c; return ioctl(fd, req, p); }
static int real_sync(void *c, int fd) { (void)c; return fsync(fd); }
static int real_map(void *c, struct ota_mtd_map *map, char *e, size_t n)
{ (void)c; return ota_mtd_read("/proc/mtd", map, e, n); }
static const struct ota_mtd_syscalls linux_calls = {
    real_open, real_close, real_stat, real_lstat, real_read, real_pread,
    real_pwrite, real_ioctl, real_sync, real_map
};
static int fail(char *e, size_t n, const char *message)
{ if (n) snprintf(e, n, "%s", message); return -1; }
static int poison(struct ota_linux_mtd *m) { m->faulted = 1; return -1; }
static int lease(struct ota_linux_mtd *m, enum ota_mtd_phase phase)
{
    char error[128];
    if (!m->active || m->faulted || !m->lease.check ||
        m->lease.check(m->lease.context, m->plan, phase, m->sync_mode) ||
        ota_apply_plan_guard(m->plan, error, sizeof error)) return poison(m);
    return 0;
}
static int ioctlinfo(struct ota_linux_mtd *m, int fd, unsigned long req, void *out)
{
    int rc;
    do { rc = m->sys->ioctl(m->sys_context, fd, req, out); } while (rc < 0 && errno == EINTR);
    return rc;
}
static int sync_fsync(struct ota_linux_mtd *m, int fd)
{
    int rc;
    do { rc = m->sys->fsync(m->sys_context, fd); } while (rc < 0 && errno == EINTR);
    return rc;
}
static int text(struct ota_linux_mtd *m, unsigned index, const char *attribute,
                const char *expected)
{
    char path[96], value[128];
    snprintf(path, sizeof path, "/sys/class/mtd/mtd%u/%s", index, attribute);
    int fd = m->sys->open(m->sys_context, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t length = 0;
    int result = 0;
    struct stat st;
    if (m->sys->fstat(m->sys_context, fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 022)) result = -1;
    while (!result && length < sizeof value) {
        ssize_t got = m->sys->read(m->sys_context, fd, value + length, sizeof value - length);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 || (size_t)got > sizeof value - length) { result = -1; break; }
        if (!got) break;
        length += (size_t)got;
    }
    if (m->sys->close(m->sys_context, fd)) result = -1;
    if (result || length != strlen(expected) || memcmp(value, expected, length)) return -1;
    return 0;
}
static int attributes(struct ota_linux_mtd *m, unsigned index)
{
    char expected[64];
    const struct ota_mtd_partition *part = &m->plan->map.part[index];
    snprintf(expected, sizeof expected, "%s\n", part->name);
    if (text(m, index, "name", expected)) return -1;
    snprintf(expected, sizeof expected, "90:%u\n", index * 2);
    if (text(m, index, "dev", expected)) return -1;
    snprintf(expected, sizeof expected, "%u\n", part->flash_offset);
    if (text(m, index, "offset", expected)) return -1;
    snprintf(expected, sizeof expected, "%u\n", part->size);
    if (text(m, index, "size", expected)) return -1;
    return 0;
}
static int character(const struct stat *st, unsigned index)
{
    return !S_ISCHR(st->st_mode) || st->st_uid != 0 || (st->st_mode & 022) ||
        major(st->st_rdev) != 90 || minor(st->st_rdev) != index * 2;
}
static int identity(const struct stat *a, const struct stat *b)
{
    return a->st_dev != b->st_dev || a->st_ino != b->st_ino || a->st_rdev != b->st_rdev ||
        a->st_mode != b->st_mode || a->st_uid != b->st_uid || a->st_gid != b->st_gid;
}
static int target(struct ota_linux_mtd *m, unsigned slot, int fd, int bound)
{
    struct stat st, named;
    struct mtd_info_user info;
    int regions = -1;
    unsigned index = slot + 3;
    memset(&info, 0, sizeof info);
    if (m->sys->fstat(m->sys_context, fd, &st) || character(&st, index) ||
        m->sys->lstat(m->sys_context, paths[slot], &named) || character(&named, index) ||
        identity(&st, &named) || (bound && identity(&st, &m->identity[slot])) ||
        attributes(m, index) || ioctlinfo(m, fd, MEMGETINFO, &info) ||
        ioctlinfo(m, fd, MEMGETREGIONCOUNT, &regions) || regions != 0 ||
        info.type != MTD_NORFLASH || info.size != m->plan->map.part[index].size ||
        info.erasesize != OTA_ERASE_SIZE || info.writesize != 1 || info.oobsize != 0 ||
        (info.flags & (MTD_WRITEABLE | MTD_BIT_WRITEABLE)) != (MTD_WRITEABLE | MTD_BIT_WRITEABLE) ||
        (info.flags & (MTD_NO_ERASE | MTD_POWERUP_LOCK))) return -1;
    if (!bound) m->identity[slot] = st;
    return 0;
}
static int geometry(void *context, struct ota_mtd_map *map)
{
    struct ota_linux_mtd *m = context;
    char error[128];
    if (lease(m, OTA_MTD_GEOMETRY) || m->sys->map(m->sys_context, map, error, sizeof error) ||
        ota_mtd_validate(map, error, sizeof error) || !ota_mtd_equal(map, &m->plan->map)) return poison(m);
    for (unsigned i = 0; i < OTA_MTD_COUNT; ++i)
        if (attributes(m, i)) return poison(m);
    for (unsigned i = 0; i < 2; ++i)
        if ((m->selected & (1U << i)) && target(m, i, m->fd[i], 1)) return poison(m);
    return 0;
}
static int selected_range(struct ota_linux_mtd *m, uint32_t index, uint32_t off, size_t size,
                           enum ota_mtd_phase phase)
{
    if (index < 3 || index > 4 || !size || size > OTA_ERASE_SIZE ||
        !(m->selected & (1U << (index - 3))) || off > m->plan->map.part[index].size ||
        size > m->plan->map.part[index].size - off || lease(m, phase) ||
        target(m, index - 3, m->fd[index - 3], 1)) return poison(m);
    return 0;
}
static int prepare_sync(struct ota_linux_mtd *m, unsigned slot)
{
    if (m->sync_mode != OTA_MTD_SYNC_RELEASE49 || m->sync_fd[slot] >= 0) return 0;
    /* A fresh open file description, never dup(): its final close invokes
     * Linux 4.9 mtdchar_close while the primary identity FD remains held. */
    int fd = m->sys->open(m->sys_context, paths[slot], O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_SYNC);
    if (fd < 0) return -1;
    if (fd == m->fd[0] || fd == m->fd[1] || target(m, slot, fd, 1)) {
        m->sys->close(m->sys_context, fd); return -1;
    }
    m->sync_fd[slot] = fd;
    return 0;
}
static int synchronize(struct ota_linux_mtd *m, unsigned slot)
{
    if (!(m->dirty & (1U << slot))) return 0;
    if (lease(m, OTA_MTD_SYNC)) return -1;
    if (m->sync_mode == OTA_MTD_SYNC_FSYNC) {
        if (sync_fsync(m, m->fd[slot])) return poison(m);
    } else {
        int fd = m->sync_fd[slot];
        m->sync_fd[slot] = -1;
        /* Never retry close on EINTR: Linux consumes the descriptor. */
        if (fd < 0) return poison(m);
        int invalid = target(m, slot, fd, 1);
        int closed = m->sys->close(m->sys_context, fd);
        if (invalid || closed) return poison(m);
    }
    m->dirty &= ~(1U << slot);
    return 0;
}
static int erase(void *context, uint32_t index, uint32_t offset, uint32_t size)
{
    struct ota_linux_mtd *m = context;
    if (m->in_block || m->next_block >= m->plan->block_count) return poison(m);
    const struct ota_apply_block *block = &m->plan->blocks[m->next_block];
    if (block->mtd_index != index || block->partition_offset != offset ||
        block->erase_bytes != size || size != OTA_ERASE_SIZE || offset % OTA_ERASE_SIZE ||
        selected_range(m, index, offset, size, OTA_MTD_ERASE) || prepare_sync(m, index - 3)) return poison(m);
    struct erase_info_user request = { offset, size };
    m->dirty |= 1U << (index - 3);
    /* An interrupted mutation may already have started. Do not retry it. */
    if (m->sys->ioctl(m->sys_context, m->fd[index - 3], MEMERASE, &request)) return poison(m);
    m->in_block = 1; m->reading = 0; m->written = m->read_back = 0;
    return 0;
}
static int write_bytes(void *context, uint32_t index, uint32_t offset, const uint8_t *data, size_t size)
{
    struct ota_linux_mtd *m = context;
    if (!m->in_block || m->reading || m->next_block >= m->plan->block_count) return poison(m);
    const struct ota_apply_block *block = &m->plan->blocks[m->next_block];
    if (!data || block->mtd_index != index || offset != block->partition_offset + m->written ||
        m->written > block->data_bytes || size > block->data_bytes - m->written ||
        selected_range(m, index, offset, size, OTA_MTD_WRITE) || prepare_sync(m, index - 3)) return poison(m);
    m->dirty |= 1U << (index - 3);
    size_t done = 0;
    while (done < size) {
        if (lease(m, OTA_MTD_WRITE)) return -1;
        ssize_t put = m->sys->pwrite(m->sys_context, m->fd[index - 3], data + done, size - done, offset + done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0 || (size_t)put > size - done) return poison(m);
        done += (size_t)put;
    }
    m->written += (uint32_t)size;
    return 0;
}
static int read_bytes(void *context, uint32_t index, uint32_t offset, uint8_t *data, size_t size)
{
    struct ota_linux_mtd *m = context;
    if (!m->in_block || m->next_block >= m->plan->block_count) return poison(m);
    const struct ota_apply_block *block = &m->plan->blocks[m->next_block];
    if (!data || block->mtd_index != index || m->written != block->data_bytes ||
        offset != block->partition_offset + m->read_back || size > OTA_ERASE_SIZE - m->read_back ||
        selected_range(m, index, offset, size, OTA_MTD_READ) || synchronize(m, index - 3)) return poison(m);
    m->reading = 1;
    size_t done = 0;
    while (done < size) {
        if (lease(m, OTA_MTD_READ)) return -1;
        ssize_t got = m->sys->pread(m->sys_context, m->fd[index - 3], data + done, size - done, offset + done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0 || (size_t)got > size - done) return poison(m);
        done += (size_t)got;
    }
    m->read_back += (uint32_t)size;
    if (m->read_back == OTA_ERASE_SIZE) { m->in_block = 0; m->next_block++; }
    return 0;
}
int ota_linux_mtd_close(struct ota_linux_mtd *m)
{
    int result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (m->sync_fd[i] >= 0) {
            int fd = m->sync_fd[i]; m->sync_fd[i] = -1;
            if (m->sys->close(m->sys_context, fd)) result = -1;
        }
        if (m->fd[i] >= 0) {
            int fd = m->fd[i]; m->fd[i] = -1;
            if (m->sys->close(m->sys_context, fd)) result = -1;
        }
    }
    m->active = 0;
    return result;
}
int ota_linux_mtd_finish(struct ota_linux_mtd *m, char *error, size_t size)
{
    int result = !m->active || m->faulted || m->in_block || m->next_block != m->plan->block_count;
    if (!result) {
        struct ota_mtd_map map;
        if (geometry(m, &map)) result = -1;
        for (unsigned i = 0; !result && i < 2; ++i)
            if ((m->selected & (1U << i)) && synchronize(m, i)) result = -1;
    }
    if (ota_linux_mtd_close(m)) result = -1;
    return result ? fail(error, size, "MTD finalization failed; target may be partially changed") : 0;
}
int ota_linux_mtd_open(struct ota_linux_mtd *m, struct ota_apply_plan *plan,
                       const struct ota_mtd_lease *isolation, enum ota_mtd_sync mode,
                       const struct ota_mtd_syscalls *sys, void *context,
                       struct ota_apply_backend *backend, char *error, size_t size)
{
    memset(m, 0, sizeof *m);
    m->fd[0] = m->fd[1] = m->sync_fd[0] = m->sync_fd[1] = -1;
    if (backend) memset(backend, 0, sizeof *backend);
    m->sys = sys ? sys : &linux_calls; m->sys_context = context;
    if (!plan || !backend || !isolation || !isolation->check ||
        (mode != OTA_MTD_SYNC_FSYNC && mode != OTA_MTD_SYNC_RELEASE49) ||
        !m->sys->open || !m->sys->close || !m->sys->fstat || !m->sys->lstat ||
        !m->sys->read || !m->sys->pread || !m->sys->pwrite || !m->sys->ioctl ||
        !m->sys->fsync || !m->sys->map)
        return fail(error, size, "complete verified isolation lease, sync policy and syscall table required");
    m->plan = plan; m->lease = *isolation; m->sync_mode = mode; m->active = 1;
    struct ota_verified verified;
    if (lease(m, OTA_MTD_OPEN) || ota_mtd_validate(&plan->map, error, size) ||
        ota_verify_fd(plan->capsule_fd, plan->key, plan->accepted, &verified, error, size) ||
        memcmp(verified.header, plan->verified.header, OTA_HEADER_SIZE) ||
        !plan->block_count || plan->block_count > OTA_PLAN_BLOCKS) goto failed;
    for (unsigned i = 0; i < plan->block_count; ++i) {
        unsigned index = plan->blocks[i].mtd_index;
        if (index != 3 && index != 4) goto failed;
        m->selected |= 1U << (index - 3);
    }
    struct ota_mtd_map map;
    if (m->sys->map(context, &map, error, size) || ota_mtd_validate(&map, error, size) ||
        !ota_mtd_equal(&map, &plan->map)) goto failed;
    for (unsigned i = 0; i < OTA_MTD_COUNT; ++i) if (attributes(m, i)) goto failed;
    for (unsigned i = 0; i < 2; ++i) {
        if (!(m->selected & (1U << i))) continue;
        if (lease(m, OTA_MTD_OPEN)) goto failed;
        m->fd[i] = m->sys->open(context, paths[i], O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_SYNC);
        if (m->fd[i] < 0 || target(m, i, m->fd[i], 0) ||
            (mode == OTA_MTD_SYNC_FSYNC ? sync_fsync(m, m->fd[i]) : prepare_sync(m, i))) goto failed;
    }
    *backend = (struct ota_apply_backend){ m, geometry, erase, write_bytes, read_bytes };
    return 0;
failed:
    m->faulted = 1; ota_linux_mtd_close(m);
    return fail(error, size, "MTD activation checks failed before erase/write");
}
