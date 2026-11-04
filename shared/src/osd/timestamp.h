#ifndef CAMERAHACK_OSD_TIMESTAMP_H
#define CAMERAHACK_OSD_TIMESTAMP_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#define CAMERA_OSD_TIMESTAMP_CHARS 23u
#define CAMERA_OSD_TIMESTAMP_WIDTH 418u
#define CAMERA_OSD_TIMESTAMP_HEIGHT 26u
/* Caller supplies its cached timezone-converted civil time. This module never
 * modifies TZ, locale or wall clock. English weekdays are intentional. */
int camera_osd_format_time(const struct tm *,char out[24]);
/* Device-independent bitmap: 0 transparent,1 outline,2 foreground. This is NOT
 * an assertion of the FH8626 hardware pixel/alpha representation. */
int camera_osd_timestamp_mask(const char text[24],uint8_t *out,size_t bytes,size_t stride);
/* Exact stock2-bit packing: four indices per byte, leftmost in low two bits. */
int camera_osd_timestamp_pack(const uint8_t *mask,size_t mask_bytes,size_t mask_stride,
                              uint8_t *out,size_t out_bytes,size_t out_stride);
#endif
