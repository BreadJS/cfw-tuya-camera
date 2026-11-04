/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_ENCODER_CONTROL_H
#define CAMERA_ANYKA_ENCODER_CONTROL_H
#include "sdk_abi.h"
#include <stdint.h>
struct anyka_encoder_control;
#define ANYKA_ENCODER_BITRATE 1u
#define ANYKA_ENCODER_GOP 2u
#define ANYKA_ENCODER_QP 4u
#define ANYKA_ENCODER_RATE_CONTROL 8u
#define ANYKA_ENCODER_ALL 15u
#define ANYKA_ENCODER_IFRAME_MAX_KBYTES 480u
struct anyka_encoder_settings {
    unsigned target_kbps,maximum_kbps,gop_frames;
    unsigned initial_qp,minimum_qp,maximum_qp;
    unsigned rate_control; /* Exact target SDK: 0 CBR,1 VBR. */
};
struct anyka_encoder_iframe {
    unsigned minimum_qp,maximum_qp,minimum_kbytes,maximum_kbytes;
};
struct anyka_encoder_snapshot {
    struct anyka_encode_param attributes;
    struct anyka_encoder_settings settings;
    struct anyka_encoder_iframe iframe_effective,iframe_requested;
    unsigned iframe_valid,iframe_request_known,iframe_adjusted;
    unsigned long changes,idr_requests;
    int error,sdk_result;
};
/* Caller already exclusively owns the encoder from media_backend. No encoder
 * open/close, constructor or independent capture initialization occurs here.
 * Every invocation must be serialized by that owner's executor. */
int anyka_encoder_control_open(struct anyka_encoder_control **,unsigned handle);
int anyka_encoder_control_read(struct anyka_encoder_control *,struct anyka_encoder_snapshot *);
/* Get-copy-modify-set; only selected proven fields change. Width,height,FPS,
 * profile,codec,smart parameters and every other returned byte are preserved.
 * SDK readback is a configuration echo, not proof of bitstream behavior. */
int anyka_encoder_control_apply(struct anyka_encoder_control *,unsigned fields,
    const struct anyka_encoder_settings *,struct anyka_encoder_snapshot *);
/* Explicit complete I-frame adjustment. Both QPs0 means automatic; both sizes0
 * means automatic. Getter cannot reveal original automatic/manual intent, so
 * this separate operation is never called implicitly by ordinary settings. */
int anyka_encoder_control_iframe(struct anyka_encoder_control *,
    const struct anyka_encoder_iframe *,struct anyka_encoder_snapshot *);
int anyka_encoder_control_idr(struct anyka_encoder_control *);
int anyka_encoder_request_idr(struct anyka_encoder_control *);
/* Frees this small binding only. Does not release SDK/hardware ownership. */
void anyka_encoder_control_free(struct anyka_encoder_control *);
#endif
