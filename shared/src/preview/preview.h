#ifndef CAMERA_PREVIEW_H
#define CAMERA_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
#define PREVIEW_MAX_AU (512u*1024u)
#define PREVIEW_MAX_CLIENTS 2u
struct preview;
struct preview_config {
    unsigned width,height;
    const uint8_t *sps,*pps; /* NAL bytes without Annex-B start codes. */
    size_t sps_size,pps_size;
};
/* Copies SPS/PPS into a bounded hub. No capture, device or listener opens.
 * Match camera_media_description's SPS/PPS and actual encoded dimensions.
 * Input contract: H264 IP-only, no B/reordered pictures (PTS equals DTS). */
struct preview *preview_create(const struct preview_config *);
/* Call after a successful native video callback with camera_media_frame's
 * complete Annex-B AU, source pts_us and keyframe flag. Copies synchronously;
 * does not retain capture-provider memory or wait for any network/client lock.
 * 0=accepted, 1=dropped/resynchronizing, negative=invalid/unsupported SPS change.
 * A missing AU invalidates reference chains: existing clients reconnect and
 * new output resumes only at an IDR. A bounded PPS change rebuilds avcC and reconnects old-init clients;
 * an SPS change is rejected (geometry/profile remain fixed). */
int preview_publish(struct preview *,const uint8_t *,size_t,uint64_t pts_us,int keyframe);
/* Authenticated HTTP route hands off its already accepted nonblocking socket.
 * 0=ownership transferred to a bounded detached worker; negative=not owned.
 * Worker writes HTTP200 video/mp4 + ftyp/moov + moof/mdat, close-delimited.
 * At most two clients; new clients wait for a future IDR. Lost references,
 * source discontinuity or a slow reader close the stream for browser reconnect.
 * HTTP auth/CSRF/origin and route selection remain the caller's responsibility. */
int preview_stream(struct preview *,int accepted_fd);
/* Immutable (SPS/profile fixed) for hub lifetime, suitable for preview/status JSON and MSE
 * isTypeSupported/addSourceBuffer. Example video/mp4; codecs="avc1.4D0029". */
const char *preview_mime(const struct preview *);
/* Stop publisher first. Shutdown workers and release memory, bounded by their
 * poll/send deadlines. No fd or hardware teardown outside owned clients. */
void preview_destroy(struct preview *);
#endif
