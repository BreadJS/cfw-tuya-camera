#ifndef CAMERHACK_FH_AUDIO_H
#define CAMERHACK_FH_AUDIO_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FH_AUDIO_MAX_BYTES 8192U
#define FH_AUDIO_CONFIG_BYTES 32U
#define FH_AUDIO_PTS_BYTES 16U
enum fh_audio_state { FH_AUDIO_NEW, FH_AUDIO_OPEN, FH_AUDIO_CONFIGURED,
                      FH_AUDIO_RUNNING, FH_AUDIO_STOPPED, FH_AUDIO_CLOSED };
enum fh_audio_phase { FH_AUDIO_LEASE_OPEN, FH_AUDIO_LEASE_CONFIGURE,
    FH_AUDIO_LEASE_START, FH_AUDIO_LEASE_READ, FH_AUDIO_LEASE_STOP, FH_AUDIO_LEASE_CLOSE };
struct fh_audio;
struct fh_audio_lease {
    void *opaque;
    dev_t device; /* Independently verified /dev/fh_audio character dev_t. */
    /* Mandatory trusted owner code: exclusive audio/media ownership, exact
     * driver identity, no other reader/configurer (including dgiot), and a
     * dedicated blocking producer thread. CLOSE after enable additionally
     * requires owner-verified quiescence; disable ACK alone is insufficient. */
    int (*check)(void *, const struct fh_audio *, enum fh_audio_phase);
};
struct fh_audio_io {
    void *opaque;
    int (*open)(void *, const char *, int); /* owned fd or negative errno */
    int (*close)(void *, int); /* Consumes owned fd even on Linux EINTR/error. */
    int (*fstat)(void *, int, struct stat *);
    int (*lstat)(void *, const char *, struct stat *);
    int (*ioctl)(void *, int, uint32_t command, uint8_t *wire, size_t bytes);
    ssize_t (*read)(void *, int, void *, size_t);
    int (*pointer32)(void *, void *, uint32_t *); /* Native ptr -> ARM user ptr. */
};
struct fh_audio_frame {
    size_t bytes; /* May be zero or short, including a partial PCM sample. */
    unsigned has_raw_pts;
    uint32_t raw_pts[2]; /* Exact returned words at offsets 8/12; units unknown. */
};
struct fh_audio {
    pthread_mutex_t mutex;
    pthread_t producer;
    struct fh_audio_lease lease;
    struct fh_audio_io io;
    enum fh_audio_state state;
    struct stat identity;
    int fd, initialized, producer_bound, fault, last_error, last_close_error;
    unsigned configure_attempted, enable_attempted, enable_ack, disable_attempted, disable_ack;
    unsigned exclusive_prepare_attempted, exclusive_prepare_ack;
    uint32_t words[7], request_bytes;
    uint8_t config_request[32], config_reply[32];
    void *mapping;
    size_t mapping_bytes;
    uint8_t *scratch; /* Private 8192-byte destination, surrounded by guard pages. */
};
void fh_audio_stock_config(uint32_t words[7]);
/* Narrow supported policy: input type0, PCM mono16, rates8/16/32k (candidates,
 * not hardware acceptance), periods80..1024; >100 volume maps to85 as stock.
 * Seven user words -> exact observed eight-word little-endian request. */
int fh_audio_pack_config(const uint32_t words[7], uint8_t wire[32], uint32_t *read_bytes);
/* Fresh storage only. NULL io selects actual Linux calls; no hardware call is
 * made by init. No production lease implementation/default success exists. */
int fh_audio_init(struct fh_audio *, const struct fh_audio_lease *, const struct fh_audio_io *);
int fh_audio_open(struct fh_audio *);
/* Narrow handoff operation: fresh private OPEN, not configured/enabled/faulted.
 * Under the STOP ownership gate, sends exact stock input-disable ioctl7 once
 * to request that inherited remote input be stopped before configuration.
 * Remains OPEN; separate attempt/ACK flags, no normal stop/enable flags, frees,
 * fd close, RUNNING fabrication or full DMA-quiescence assertion. Failure faults
 * the object and preserves exact status. ACK is not cleanup permission. */
int fh_audio_prepare_exclusive_input(struct fh_audio *);
int fh_audio_configure(struct fh_audio *, const uint32_t words[7]);
int fh_audio_start(struct fh_audio *);
/* Called by the dedicated producer thread after start, before any read. */
int fh_audio_bind_producer(struct fh_audio *);
/* BLOCKING vendor RPC, with no verified poll/nonblocking/timeout/cancel path.
 * Do not call from an RTSP event-loop callback. The owner supplies a bounded
 * queue between this producer and RTSP. No automatic read/ioctl retry. */
int fh_audio_read(struct fh_audio *, void *out, size_t capacity, unsigned with_pts,
                  struct fh_audio_frame *);
/* Stop/close return -EBUSY during a read and never close an in-flight fd.
 * Stop ioctl7 success is an ACK, not proven DMA/service quiescence. Stop may
 * be explicitly retried after uncertain enable/disable or capture errors. */
int fh_audio_stop(struct fh_audio *);
int fh_audio_close(struct fh_audio *);
int fh_audio_destroy(struct fh_audio *); /* Refuses while any owned fd remains. */
const struct fh_audio_io *fh_audio_linux_io(void);
#endif
