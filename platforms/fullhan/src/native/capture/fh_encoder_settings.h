#ifndef CAMERA_FH_ENCODER_SETTINGS_H
#define CAMERA_FH_ENCODER_SETTINGS_H
#include "fh_capture.h"
#define FH_ENCODER_SETTINGS_BYTES 64u
#define FH_ENCODER_RATE_FIXED_QP 2u
struct fh_encoder_settings {
 uint32_t width,height,fps,gop,profile,qp,i_qp,p_qp,rate_mode,bitrate_kbps;
};
enum fh_encoder_change {
 FH_ENCODER_GEOMETRY=1u,FH_ENCODER_FPS=2u,FH_ENCODER_GOP=4u,
 FH_ENCODER_PROFILE=8u,FH_ENCODER_QUANTIZERS=16u,FH_ENCODER_RATE=32u,
 FH_ENCODER_ALL=63u
};
struct fh_encoder_plan {
 struct fh_capture_config capture;
 unsigned changed,restart_required;
};
void fh_encoder_settings_defaults(struct fh_encoder_settings *);
/* Supported combinations:1080p1..15fps or720p30, fixedQP only. Profile66/77,
 * GOP1..65535, independent initial/I/P quantizers0..51. All changes require
 * a cold pipeline start; this module never mutates a live capture or an fd.
 * Wire-supported values are not a guarantee of bitrate, quality or cadence.
 * Unsupported rate mode/nonzero bitrate returns-ENOTSUP; no guessed bitrate
 * mode or driver structure is constructed. */
int fh_encoder_settings_validate(const struct fh_encoder_settings *);
/* active=NULL marks all fields changed. On success produces only startup
 * config; geometry still uses the existing1080p backing/clock/VMM lifecycle.
 *720p30 sets the existing explicit experimental_fps30 flag. */
int fh_encoder_settings_plan(const struct fh_encoder_settings *desired,const struct fh_encoder_settings *active,struct fh_encoder_plan *);
/* Canonical64B little-endian version1 record with magic/reserved/CRC32.
 * CRC detects accidental corruption, not authenticity. Owner handles storage,
 * atomic persistence, authentication and restart. Invalid inputs never mutate
 * caller output. Encoding accepts capacity>=64; decoding requires exactly64. */
int fh_encoder_settings_encode(const struct fh_encoder_settings *,uint8_t *,size_t capacity);
int fh_encoder_settings_decode(const uint8_t *,size_t bytes,struct fh_encoder_settings *);
const char *fh_encoder_bitrate_unsupported_reason(void);
const char *fh_encoder_live_unsupported_reason(void);
#endif
