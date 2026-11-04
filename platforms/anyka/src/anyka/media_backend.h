/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_MEDIA_BACKEND_H
#define CAMERA_ANYKA_MEDIA_BACKEND_H
#include "../native/rtsp/camera_rtsp.h"
#include "video_mode.h"

struct anyka_media_backend;
/* Experimental SDK-host only. Requires the explicit RTSP trial environment,
 * fail-closed host, exact SDK versions, DMA budget and no competing owner.
 * Opens VI and H264 only; no audio yet. No flash/configuration writes.
 *
 * On failure, *out may retain a backend whose hardware was attempted. It MUST
 * remain alive until a cold board reboot; there is no guessed SDK teardown.
 */
int anyka_media_open(struct anyka_media_backend **out, struct camera_media_provider *,
                     char *error, size_t error_capacity);
int anyka_media_open_mode(struct anyka_media_backend **,struct camera_media_provider *,
 const struct anyka_video_mode *,char *error,size_t capacity);
unsigned anyka_media_hardware_attempted(const struct anyka_media_backend *);
int anyka_media_error(const struct anyka_media_backend *);
/* Get-only owner handle; no SDK calls, creation or teardown. */
int anyka_media_encoder_handle(const struct anyka_media_backend *);
uint64_t anyka_media_delivered(const struct anyka_media_backend *);
#endif
