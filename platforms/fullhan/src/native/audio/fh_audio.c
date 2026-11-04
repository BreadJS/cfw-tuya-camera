#define _GNU_SOURCE
#include "fh_audio.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct frame_wire { uint32_t count, user, pts[2]; };
_Static_assert(sizeof(struct frame_wire) == FH_AUDIO_PTS_BYTES, "audio frame wire ABI");
_Static_assert(offsetof(struct frame_wire, pts) == 8, "audio timestamp ABI");
_Static_assert(sizeof(uint32_t[8]) == FH_AUDIO_CONFIG_BYTES, "audio config ABI");
static const char path[] = "/dev/fh_audio";
static void put32(uint8_t *p, uint32_t n)
{ p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24; }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void fh_audio_stock_config(uint32_t words[7])
{ const uint32_t stock[7] = { 0, 8000, 16, 0, 1, 256, 100 }; memcpy(words, stock, sizeof stock); }
int fh_audio_pack_config(const uint32_t w[7], uint8_t wire[32], uint32_t *bytes)
{
    if (!w || !wire || !bytes) return -EINVAL;
    if (w[0] != 0 || (w[1] != 8000 && w[1] != 16000 && w[1] != 32000) ||
        w[2] != 16 || w[3] != 0 || w[4] != 1) return -ENOTSUP;
    if (w[5] < 80 || w[5] > 1024) return -EINVAL;
    /* Stock c9584: channels@16, ring@20, period@24, encoding@28. */
    uint32_t kernel[8] = { w[0], w[6] > 100 ? 85 : w[6], w[1], w[2],
                           w[4], w[5] * 8, w[5], w[3] };
    for (unsigned i = 0; i < 8; ++i) put32(wire + 4 * i, kernel[i]);
    *bytes = w[5] * 2;
    return 0;
}
static int enter(struct fh_audio *a)
{
    if (!a || !a->initialized) return -EINVAL;
    int rc = pthread_mutex_trylock(&a->mutex);
    return rc ? (rc == EBUSY ? -EBUSY : -rc) : 0;
}
static int leave(struct fh_audio *a, int rc)
{ a->last_error = rc; pthread_mutex_unlock(&a->mutex); return rc; }
static int character(const struct fh_audio *a, const struct stat *st)
{ return !S_ISCHR(st->st_mode) || st->st_uid != 0 || (st->st_mode & 022) || st->st_rdev != a->lease.device; }
static int different(const struct stat *x, const struct stat *y)
{ return x->st_dev != y->st_dev || x->st_ino != y->st_ino || x->st_rdev != y->st_rdev ||
         x->st_mode != y->st_mode || x->st_uid != y->st_uid || x->st_gid != y->st_gid; }
static int gate(struct fh_audio *a, enum fh_audio_phase phase)
{
    int rc = a->lease.check(a->lease.opaque, a, phase);
    if (rc) return rc;
    if (a->fd >= 0) {
        struct stat retained, named;
        if (a->io.fstat(a->io.opaque, a->fd, &retained) || a->io.lstat(a->io.opaque, path, &named) ||
            character(a, &retained) || character(a, &named) || different(&retained, &named) ||
            different(&retained, &a->identity)) return -ESTALE;
    }
    return 0;
}
int fh_audio_init(struct fh_audio *a, const struct fh_audio_lease *lease, const struct fh_audio_io *io)
{
    if (!a || !lease || !lease->device || !lease->check) return -EINVAL;
    if (!io) io = fh_audio_linux_io();
    if (!io->open || !io->close || !io->fstat || !io->lstat || !io->ioctl || !io->read || !io->pointer32) return -EINVAL;
    memset(a, 0, sizeof *a); a->fd = -1; a->lease = *lease; a->io = *io;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page > 1048576) return -ENOTSUP;
    size_t middle = ((FH_AUDIO_MAX_BYTES + (size_t)page - 1) / (size_t)page) * (size_t)page;
    a->mapping_bytes = middle + 2 * (size_t)page;
    a->mapping = mmap(NULL, a->mapping_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a->mapping == MAP_FAILED) { a->mapping = NULL; return -errno; }
    a->scratch = (uint8_t *)a->mapping + page;
    if (mprotect(a->scratch, middle, PROT_READ | PROT_WRITE)) {
        int rc = -errno; munmap(a->mapping, a->mapping_bytes); return rc;
    }
    /* Place destination immediately before the trailing guard, even on a
     * system whose pages exceed8192. Driver ioctl23 does not re-clamp RPC
     * copy length after the exchange; a bad length must not overwrite ctx. */
    a->scratch += middle - FH_AUDIO_MAX_BYTES;
    int rc = pthread_mutex_init(&a->mutex, NULL);
    if (rc) { munmap(a->mapping, a->mapping_bytes); return -rc; }
    a->initialized = 1; return 0;
}
int fh_audio_open(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->state != FH_AUDIO_NEW || a->fd >= 0 || a->fault) return leave(a, -EINVAL);
    if ((rc = gate(a, FH_AUDIO_LEASE_OPEN))) return leave(a, rc);
    struct stat named, opened;
    if ((rc = a->io.lstat(a->io.opaque, path, &named))) return leave(a, rc);
    if (character(a, &named)) return leave(a, -ENODEV);
    int fd = a->io.open(a->io.opaque, path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { a->fault = 1; return leave(a, fd); }
    rc = a->io.fstat(a->io.opaque, fd, &opened);
    if (rc || character(a, &opened) || different(&named, &opened)) {
        a->last_close_error = a->io.close(a->io.opaque, fd);
        a->fault = 1;
        return leave(a, rc ? rc : -ESTALE);
    }
    a->fd = fd; a->identity = opened; a->state = FH_AUDIO_OPEN;
    return leave(a, 0);
}
int fh_audio_prepare_exclusive_input(struct fh_audio *a)
{
    int rc=enter(a); if(rc)return rc;
    if(a->state!=FH_AUDIO_OPEN || a->fd<0 || a->fault || a->configure_attempted ||
       a->enable_attempted || a->exclusive_prepare_attempted)return leave(a,-EINVAL);
    if((rc=gate(a,FH_AUDIO_LEASE_STOP)))return leave(a,rc);
    a->exclusive_prepare_attempted=1;
    rc=a->io.ioctl(a->io.opaque,a->fd,7,NULL,0);
    if(rc)a->fault=1;else a->exclusive_prepare_ack=1;
    return leave(a,rc);
}
int fh_audio_configure(struct fh_audio *a, const uint32_t words[7])
{
    uint8_t request[32]; uint32_t bytes;
    int rc = fh_audio_pack_config(words, request, &bytes); if (rc) return rc;
    rc = enter(a); if (rc) return rc;
    if (a->state != FH_AUDIO_OPEN || a->fault) return leave(a, -EINVAL);
    if ((rc = gate(a, FH_AUDIO_LEASE_CONFIGURE))) return leave(a, rc);
    memcpy(a->config_request, request, 32); memcpy(a->config_reply, request, 32);
    memcpy(a->words, words, sizeof a->words); a->request_bytes = bytes; a->configure_attempted = 1;
    rc = a->io.ioctl(a->io.opaque, a->fd, 0, a->config_reply, 32);
    if (!rc && memcmp(a->config_reply, request, 32)) rc = -EPROTO;
    if (rc) a->fault = 1; else a->state = FH_AUDIO_CONFIGURED;
    return leave(a, rc);
}
int fh_audio_start(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->state != FH_AUDIO_CONFIGURED || a->fault || a->enable_attempted) return leave(a, -EINVAL);
    if ((rc = gate(a, FH_AUDIO_LEASE_START))) return leave(a, rc);
    a->enable_attempted = 1;
    rc = a->io.ioctl(a->io.opaque, a->fd, 5, NULL, 0);
    if (rc) a->fault = 1;
    else { a->enable_ack = 1; a->state = FH_AUDIO_RUNNING; }
    return leave(a, rc);
}
int fh_audio_bind_producer(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->state != FH_AUDIO_RUNNING || a->fault) return leave(a, -EINVAL);
    if (a->producer_bound && !pthread_equal(a->producer, pthread_self())) return leave(a, -EPERM);
    if ((rc = gate(a, FH_AUDIO_LEASE_READ))) return leave(a, rc);
    a->producer = pthread_self(); a->producer_bound = 1;
    return leave(a, 0);
}
int fh_audio_read(struct fh_audio *a, void *out, size_t capacity, unsigned with_pts, struct fh_audio_frame *frame)
{
    if (!out || !frame || !capacity || capacity > FH_AUDIO_MAX_BYTES || with_pts > 1) return -EINVAL;
    memset(frame, 0, sizeof *frame);
    int rc = enter(a); if (rc) return rc;
    if (a->state != FH_AUDIO_RUNNING || a->fault || !a->producer_bound) return leave(a, -EINVAL);
    if (!pthread_equal(a->producer, pthread_self())) return leave(a, -EPERM);
    if (capacity < a->request_bytes) return leave(a, -EMSGSIZE);
    if ((rc = gate(a, FH_AUDIO_LEASE_READ))) return leave(a, rc);
    size_t count;
    if (with_pts) {
        uint8_t wire[16] = {0}; uint32_t address;
        if ((rc = a->io.pointer32(a->io.opaque, a->scratch, &address))) return leave(a, rc);
        if (!address) return leave(a, -EFAULT);
        put32(wire, a->request_bytes); put32(wire + 4, address);
        rc = a->io.ioctl(a->io.opaque, a->fd, 23, wire, sizeof wire);
        if (rc) { a->fault = 1; return leave(a, rc); }
        count = get32(wire);
        if (count > a->request_bytes || count > capacity || count > FH_AUDIO_MAX_BYTES) {
            a->fault = 1; return leave(a, -EPROTO);
        }
        /* Offset4 may now contain a driver physical address. Never trust or
         * dereference it; the sole PCM destination remains our own scratch. */
        frame->has_raw_pts = 1;
        frame->raw_pts[0] = get32(wire + 8); frame->raw_pts[1] = get32(wire + 12);
    } else {
        ssize_t got = a->io.read(a->io.opaque, a->fd, a->scratch, a->request_bytes);
        if (got < 0) return leave(a, (int)got); /* No automatic EINTR/retry. */
        if ((size_t)got > a->request_bytes || (size_t)got > capacity) {
            a->fault = 1; return leave(a, -EPROTO);
        }
        count = (size_t)got;
    }
    memcpy(out, a->scratch, count); frame->bytes = count;
    return leave(a, 0);
}
int fh_audio_stop(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->fd < 0 || !a->enable_attempted) return leave(a, -EINVAL);
    if (a->disable_ack) return leave(a, 0);
    if ((rc = gate(a, FH_AUDIO_LEASE_STOP))) return leave(a, rc);
    a->disable_attempted = 1;
    rc = a->io.ioctl(a->io.opaque, a->fd, 7, NULL, 0);
    if (rc) a->fault = 1;
    else { a->disable_ack = 1; a->state = FH_AUDIO_STOPPED; }
    return leave(a, rc);
}
int fh_audio_close(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->fd < 0) return leave(a, -EINVAL);
    if (a->enable_attempted && !a->disable_ack) return leave(a, -EBUSY);
    if ((rc = gate(a, FH_AUDIO_LEASE_CLOSE))) return leave(a, rc);
    int fd = a->fd; a->fd = -1;
    rc = a->io.close(a->io.opaque, fd); a->last_close_error = rc;
    a->state = FH_AUDIO_CLOSED; /* Consumed even if close reports EINTR. */
    return leave(a, rc);
}
int fh_audio_destroy(struct fh_audio *a)
{
    int rc = enter(a); if (rc) return rc;
    if (a->fd >= 0) return leave(a, -EBUSY);
    if (munmap(a->mapping, a->mapping_bytes)) return leave(a, -errno);
    a->mapping = NULL; a->scratch = NULL;
    pthread_mutex_unlock(&a->mutex); pthread_mutex_destroy(&a->mutex);
    a->initialized = 0; return 0;
}
