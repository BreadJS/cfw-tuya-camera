#ifndef CAMERA_RTSP_H
#define CAMERA_RTSP_H

#include <stddef.h>
#include <stdint.h>

#define CAMERA_RTSP_MAX_AU (512u * 1024u) /* Matches owned capture application buffer. */
#define CAMERA_RTSP_MAX_AUDIO 4096u
#define CAMERA_RTSP_MAX_PARAMETER_SET 256u

enum camera_audio_codec { CAMERA_AUDIO_NONE, CAMERA_AUDIO_PCMA, CAMERA_AUDIO_L16 };

/* Source memory remains owned by provider until its next callback of that type.
 * H264 data is one complete Annex-B access unit. pts_us uses CLOCK_MONOTONIC,
 * not a guessed frame counter; all NALs/fragments get one 90kHz RTP timestamp.
 * L16 audio bytes are network big-endian signed PCM. PCMA is one byte/sample.
 * Callbacks must be nonblocking: 1=data, 0=not ready, -1=source failure.
 */
struct camera_media_frame {
    const uint8_t *data;
    size_t size;
    uint64_t pts_us;
    int keyframe;
    /* Optional exact audio sampling clock. A zero rate retains legacy PTS
     * mapping. Position counts captured samples, including known source gaps;
     * pts_us still anchors that sample to the shared monotonic clock. */
    uint64_t sample_position;
    unsigned sample_clock_rate;
};

struct camera_media_description {
    const uint8_t *sps;
    size_t sps_size;
    const uint8_t *pps;
    size_t pps_size;
    enum camera_audio_codec audio_codec;
    unsigned audio_rate;
};

struct camera_media_provider {
    void *opaque;
    struct camera_media_description description;
    int (*video)(void *, uint64_t now_us, struct camera_media_frame *);
    int (*audio)(void *, uint64_t now_us, struct camera_media_frame *);
};

struct camera_rtsp_config {
    const char *bind_address; /* IPv4 literal; default is loopback. */
    unsigned port;
    const char *path;         /* One endpoint, e.g. /main; no trailing slash. */
    const char *credentials;  /* user:password, or NULL only on loopback. */
    unsigned max_clients;     /* 1..4 */
    unsigned slow_timeout_ms;
};

/* Runs until SIGINT/SIGTERM, then closes all descriptors. Does not initialize
 * hardware, write flash, contact clouds, or manage the capture process. */
int camera_rtsp_run(const struct camera_rtsp_config *, struct camera_media_provider *);
uint64_t camera_monotonic_us(void);

#endif
