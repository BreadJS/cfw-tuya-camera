#ifndef CAMERHACK_FH_VIDEO_PROVIDER_H
#define CAMERHACK_FH_VIDEO_PROVIDER_H
#include "camera_rtsp.h"
#include "../capture/fh_frame.h"

/* Native video-only adapter. No device opens, file-fixture source, timestamp
 * inference, hardware teardown or queue-release implementation is provided.
 * The caller serializes reader/capture/all mapping lifetime and must not read
 * the reader separately after init. Keep this object at a stable address. */
struct fh_video_provider {
    struct fh_frame_reader *reader;
    struct fh_frame_clock clock;
    uint8_t sps[CAMERA_RTSP_MAX_PARAMETER_SET];
    uint8_t pps[CAMERA_RTSP_MAX_PARAMETER_SET];
    size_t sps_bytes,pps_bytes;
    struct fh_frame first;
    uint64_t last_us;
    unsigned initialized,first_pending,delivered,faulted;
    int last_error;
};
/* Fresh storage only. first must be the actual already-copied/released AU from
 * reader, with both SPS/PPS and a valid owner-converted nanosecond timestamp.
 * reader.clock.raw_to_ns must implement a separately validated raw ->
 * CLOCK_MONOTONIC-domain conversion. Presence of a callback is a required
 * contract, not proof of its units/calibration. No default/dequeue clock exists.
 * SPS/PPS are copied into this object's owned <=256-byte arrays. AU bytes stay
 * in reader's owned application buffer until the next video callback.
 * Returns zero or a negative error; does not poll or mutate hardware. */
int fh_video_provider_init(struct fh_video_provider *,struct fh_frame_reader *,
                            const struct fh_frame *first,struct camera_media_provider *);
/* camera_media_provider.video callback: 1=owned AU,0=empty,-1=fault. now_us is
 * ignored: all PTS come solely from the validated reader clock, floor(ns/1000),
 * and must strictly increase in microseconds. Initial AU is delivered once
 * before another queue poll. Changed parameter sets require restart/reinit.
 * Own validation failures occur after successful queue release. Lower-layer
 * ambiguous acquire/release errors retain reader quarantine/ownership exactly
 * as reported; this adapter never retries or guesses a release/cleanup. */
int fh_video_provider_video(void *,uint64_t now_us,struct camera_media_frame *);
#endif
