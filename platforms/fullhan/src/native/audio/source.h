#ifndef CAMERHACK_AUDIO_SOURCE_H
#define CAMERHACK_AUDIO_SOURCE_H
#include "fh_audio.h"
#include "../capture/fh_frame.h"
#include "../rtsp/camera_rtsp.h"
#define FH_AUDIO_SOURCE_QUEUE 8U
#define FH_AUDIO_SOURCE_SAMPLES 256U
#define FH_AUDIO_SOURCE_WARMUP_BLOCKS 64U
enum fh_audio_source_phase { FH_AUDIO_SOURCE_INIT, FH_AUDIO_SOURCE_OPEN,
    FH_AUDIO_SOURCE_CONFIGURE, FH_AUDIO_SOURCE_ENABLE, FH_AUDIO_SOURCE_READ,
    FH_AUDIO_SOURCE_FAULT };
struct fh_audio_source;
struct fh_audio_source_policy {
    /* Explicit caller acceptance from actual microphone evidence; no default.
     * clock converts low-word/high-word rawPTS to CLOCK_MONOTONIC nanoseconds.
     * Use a dedicated audio clock instance: converter state/fault flags must not
     * share mutable storage with video. It must be nonblocking and its opaque
     * must outlive source. */
    unsigned cold_boot_verified, pcm_s16le_verified, pts_low_high_verified;
    struct fh_frame_clock clock;
};
struct fh_audio_source_status {
    enum fh_audio_source_phase phase, failed_phase;
    int error;
    unsigned done, queued;
    uint64_t blocks, samples, zero_blocks, dropped, delivered;
    uint64_t warmup_blocks, warmup_samples;
    uint32_t last_raw_pts[2];
    uint8_t config_request[32],config_reply[32];
};
/* Fresh *out=NULL. One8k/mono16 producer owns a fresh fh_audio from first open.
 * NULLio selects actual Linux backend; tests may inject complete syscall fakes.
 * No warm-disable, second open, pause/stop/close/free/cancel/detach is provided.
 * Resources remain held after failure until external verified board reset.
 * Return0 means worker created, not successful hardware startup. */
int fh_audio_source_start(struct fh_audio_source **out,const struct fh_audio_lease *,
    const struct fh_audio_source_policy *,const struct fh_audio_io *io);
/* Pure RAM callback; trylock returns0 on contention/empty,1 on an owned PCMA
 * block, -1 after producer fault once buffered blocks are drained. Buffer stays
 * valid until next callback; one serialized consumer. Source timestamps survive
 * queue drops; no arrival-time replacement. now_us ignored. */
int fh_audio_source_next(void *,uint64_t now_us,struct camera_media_frame *);
/* Trylock snapshot: -EBUSY means retry later. Never waits for vendor calls. */
int fh_audio_source_status(struct fh_audio_source *,struct fh_audio_source_status *);
uint8_t fh_audio_pcma(int16_t sample);
/* Capture and hardware timestamps continue while muted. Gain0..400percent. */
int fh_audio_source_controls(struct fh_audio_source *,unsigned enabled,unsigned gain_percent);
#endif
