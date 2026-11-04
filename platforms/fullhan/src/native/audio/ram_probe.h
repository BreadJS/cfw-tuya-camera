/* Explicit experimental microphone job; no CLI, implicit lease or cleanup. */
#ifndef CAMERHACK_AUDIO_RAM_PROBE_H
#define CAMERHACK_AUDIO_RAM_PROBE_H
#include "fh_audio.h"
#include <stdint.h>

#define FH_AUDIO_RAM_PAYLOAD_BYTES 64000U /* Fixed byte cap; deadline independent of rate. */
#define FH_AUDIO_RAM_MAX_BLOCKS 256U
enum fh_audio_ram_phase {
    FH_AUDIO_RAM_NEW, FH_AUDIO_RAM_FILES, FH_AUDIO_RAM_INIT,
    FH_AUDIO_RAM_OPEN, FH_AUDIO_RAM_CONFIGURE, FH_AUDIO_RAM_ENABLE,
    FH_AUDIO_RAM_BIND, FH_AUDIO_RAM_CLOCK_BEFORE, FH_AUDIO_RAM_BLOCKING_READ,
    FH_AUDIO_RAM_CLOCK_AFTER, FH_AUDIO_RAM_WRITE, FH_AUDIO_RAM_COMPLETE,
    FH_AUDIO_RAM_WARM_DISABLE /* Append: preserve historical phase numbers. */
};
enum fh_audio_ram_end { FH_AUDIO_RAM_ERROR, FH_AUDIO_RAM_PAYLOAD_LIMIT,
                         FH_AUDIO_RAM_SOFT_DEADLINE, FH_AUDIO_RAM_BLOCK_LIMIT };
struct fh_audio_ram_status {
    unsigned done;
    enum fh_audio_ram_phase phase;
    enum fh_audio_ram_phase last_operation;
    enum fh_audio_ram_end end;
    int result;
    uint32_t bytes, written_bytes, blocks, zero_blocks, complete_samples, odd_tail_bytes;
    int32_t minimum, maximum; /* diagnostic interpretation as signed16 LE */
    int64_t sum;
    uint64_t sum_squares, elapsed_ns;
};
struct fh_audio_ram_job;
/* *out must initially beNULL. Copies mandatory verified audio lease; caller
 * retains its opaque owner and the already-private PAE fd until external reset.
 * Creates one dedicated joinable thread; returns without microphone setup/read.
 * Real fh_audio API uses NULLio. Files: /tmp/native-audio.s16le and .log.
 * rate_hz accepts8000/16000/32000;8000 preserves stock baseline packing.
 * Other rates are candidates, not accepted rate/PCM representation.
 * After completion/error ALL job/audio fd/scratch/output resources remain held.
 * No stop/close/free/reset/detach/cancel API is provided. Do not close the PAE fd
 * or invalidate ownership while this job can block. Reset is external policy. */
int fh_audio_ram_probe_start(struct fh_audio_ram_job **out,
                              const struct fh_audio_lease *,int private_pae_fd,
                              unsigned rate_hz);
/* Default start enables the explicit pre-config inherited-input disable. This
 * variant permits only warm_disable0/1;0 preserves the earlier comparison path. */
int fh_audio_ram_probe_start_ex(struct fh_audio_ram_job **out,
                                 const struct fh_audio_lease *,int private_pae_fd,
                                 unsigned rate_hz,unsigned warm_disable);
/* Atomic, nonblocking status. While running only done=0 and phase are published;
 * final statistics become visible after done=1. No mutex/read/join is attempted.
 * A2s deadline is checked BETWEEN blocking calls; it cannot interrupt vendor RPC.
 * Status does not imply hardware stop, resource cleanup or A/V clock acceptance. */
int fh_audio_ram_probe_poll(const struct fh_audio_ram_job *,struct fh_audio_ram_status *);
#endif
