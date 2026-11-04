#ifndef CAMERA_ANYKA_AUDIO_BACKEND_H
#define CAMERA_ANYKA_AUDIO_BACKEND_H
#include "../native/rtsp/camera_rtsp.h"

#define ANYKA_AUDIO_RATE 16000u
#define ANYKA_AUDIO_CHANNELS 1u
struct anyka_audio;
struct anyka_audio_status {
    unsigned started, clock_valid, actual_rate;
    uint64_t sample_position;
    uint64_t blocks, samples;
    unsigned last_sequence;
    uint64_t last_source_ms;
    uint64_t discontinuities, last_gap_us;
    uint64_t candidate_source_ms, previous_source_ms, candidate_delta_us, candidate_now_us;
    unsigned candidate_sequence,candidate_bytes,previous_bytes;
    uint64_t candidate_expected_us;
    int64_t candidate_phase_us;
    uint64_t jittered_blocks,warmup_discarded;
    int error, sdk_result;
};

/* Caller must already own the media hardware exclusively. Resolves only the
 * exact shipped AI SDK. No constructor, service stop, driver scan or watchdog
 * operation occurs here. A returned session is retained even after a hardware
 * failure; no unverified stop/close/restart path is provided. */
int anyka_audio_open(struct anyka_audio **);
/* Thread-safe desired control. Gain percent maps to reviewed SDK gain0..8;
 * only the source callback performs SDK mutations. Mute emits silent PCM. */
int anyka_audio_control(struct anyka_audio *,unsigned enabled,unsigned percent);

/* Nonblocking camera_media_provider audio callback. Copies a complete leased
 * SDK PCM block before releasing it exactly once, then returns network-order
 * L16 mono16kHz. First block primes clock validation; two coherent SDK clock
 * samples must corroborate milliseconds before any audio is delivered.
 * Once proved, a sample lattice anchored to that shared clock smooths bounded
 * DMA-IRQ timestamp jitter; whole-period forward gaps keep source time.
 * Failure is sticky and hardware/resources remain retained until board reset. */
int anyka_audio_read(void *,uint64_t now_us,struct camera_media_frame *);
int anyka_audio_status(const struct anyka_audio *,struct anyka_audio_status *);
#endif
