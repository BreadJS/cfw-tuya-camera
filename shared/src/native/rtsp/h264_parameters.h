/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_H264_PARAMETERS_H
#define CAMERA_H264_PARAMETERS_H
#include <stddef.h>
#include <stdint.h>

/* Raw SPS NALs, including the one-byte NAL header, excluding Annex B prefix.
 * Both must be well-formed and at most 256 bytes. Decoder syntax through frame
 * cropping must be bit-identical; validated VUI/HRD metadata may change.
 * Self-comparison also validates an initial SPS. Returns 0 or negative errno. */
int camera_h264_sps_compatible(const uint8_t *old, size_t old_bytes,
                               const uint8_t *next, size_t next_bytes);
#endif
