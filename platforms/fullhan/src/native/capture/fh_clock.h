#ifndef CAMERHACK_FH_CLOCK_H
#define CAMERHACK_FH_CLOCK_H
#include "fh_frame.h"

/* Exact retained PAE dispatch; little-endian output, current low32 then high32.
 * See CLOCK_EVIDENCE.md. No device open, /dev/mem or source-PTS fallback. */
#define FH_CLOCK_CURRENT UINT32_C(0xc0085041)
#define FH_CLOCK_RATE UINT32_C(0xc0045043)
#define FH_CLOCK_REQUIRED_RATE UINT32_C(1000000)
#define FH_CLOCK_MAX_WINDOW_NS UINT64_C(1000000) /* strict <1ms */
#define FH_CLOCK_MAX_SAMPLES 16U
struct fh_clock_io {
    void *opaque;
    /* Mandatory current exclusive-owner check. The caller supplies an already
     * private, identity-validated PAE fd and retains it for this generation. */
    int (*check)(void *);
    int (*ioctl)(void *,int,uint32_t,uint8_t *,size_t);
    int (*monotonic_ns)(void *,uint64_t *);
};
struct fh_clock {
    struct fh_clock_io io;
    int pae_fd,last_error;
    uint32_t rate;
    uint64_t raw_reference_us,monotonic_reference_ns,window_ns;
    unsigned attempted,calibrated,faulted,samples;
};
/* Fresh storage only; no calls or effects. No default ownership approval. */
int fh_clock_init(struct fh_clock *,int private_pae_fd,const struct fh_clock_io *);
/* One attempt per initialized generation;2..16 back-to-back samples. Require
 * exact1MHz reported rate, ordered clocks, actual source advancement, and at
 * least one bracket strictly narrower than1ms. Keep the narrowest midpoint.
 * No sleep, estimated rate, queued-frame anchor or per-frame rebasing occurs.
 * Reset/stream discontinuity requires explicit new calibration by the owner.
 * A narrow bracket bounds sampling uncertainty, not long-run oscillator drift. */
int fh_clock_calibrate(struct fh_clock *,unsigned samples);
/* Pure fixed affine conversion plus owner check; no clock/PTS ioctl on frames.
 * Supports older queued AUs with checked subtraction, and checked forward
 * multiplication/addition. Output unchanged on error; errors fault generation. */
int fh_clock_raw_to_ns(void *,uint64_t raw,uint64_t *ns);
int fh_clock_frame_clock(struct fh_clock *,struct fh_frame_clock *);
/* Actual read-only Linux adapters for the caller's explicit io table.
 * ioctl accepts only the two exact sizes/commands; no automatic retry. */
int fh_clock_linux_ioctl(void *,int,uint32_t,uint8_t *,size_t);
int fh_clock_monotonic_ns(void *,uint64_t *);
#endif
