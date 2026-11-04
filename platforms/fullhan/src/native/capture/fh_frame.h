/* Owned H264 queue copying; no network consumer can hold a hardware buffer. */
#ifndef CAMERHACK_FH_FRAME_H
#define CAMERHACK_FH_FRAME_H
#include "fh_capture.h"
#define FH_FRAME_DESCRIPTOR_WORDS 92u
#define FH_FRAME_MAX_ENTRIES 20u
#define FH_FRAME_EMPTY 1
#define FH_FRAME_EMPTY_WIRE 0x81014084u
struct fh_frame_clock {
    void *opaque;
    /* Optional owner-validated conversion to nanoseconds. Called only after
     * successful queue release; no default units, rate or dequeue-time clock. */
    int (*raw_to_ns)(void *, uint64_t raw_pts, uint64_t *ns);
};
struct fh_frame_nal { size_t offset, bytes; }; /* excludes start-code bytes */
struct fh_frame {
    const uint8_t *data;
    size_t bytes;
    uint32_t channel, frame_class, entry_count, nal_mask;
    unsigned has_sps, has_pps, has_idr, has_vcl, timestamp_ns_valid;
    struct fh_frame_nal sps, pps;
    uint64_t raw_pts; /* Raw64-bit field; native unit/epoch remains unproven. */
    uint64_t timestamp_ns;
};
struct fh_frame_reader {
    struct fh_capture *capture;
    uint8_t *buffer;
    size_t capacity;
    struct fh_frame_clock clock;
    uint32_t descriptor[FH_FRAME_DESCRIPTOR_WORDS];
    unsigned queue_owned, ownership_uncertain, release_attempted, quarantine, busy;
    int last_acquire_result, last_copy_error, last_release_error, last_clock_error;
};
/* Fresh storage only; buffer belongs exclusively to this reader. No allocation,
 * device open, ioctl or clock call. Null clock means raw timestamps only. */
int fh_frame_reader_init(struct fh_frame_reader *, struct fh_capture *,
                          uint8_t *buffer, size_t capacity,
                          const struct fh_frame_clock *clock);
/* Return0 for an assembled Annex-B AU, FH_FRAME_EMPTY for an empty poll, or a
 * negative error. out is cleared on any non-frame result. No blocking/waits.
 * Hardware queue is released exactly once BEFORE returning data/calling clock.
 * Data remains valid until the next read; caller must copy before retaining it.
 * Caller serializes reader, capture controls, mappings and all fd lifetime.
 * Ambiguous acquire/release permanently quarantines this instance; no retry,
 * speculative release, automatic rollback or dropping allocation ownership. */
int fh_frame_read(struct fh_frame_reader *, struct fh_frame *out);
#endif
