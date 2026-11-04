/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_PICTURE_ABI_H
#define CAMERA_ANYKA_PICTURE_ABI_H
#include <stdint.h>
/* Static ABI observations ONLY, for the exact inspected SDK-host executable.
 * No binding, initialization, control implementation or hardware validation.
 * See docs/ANYKA-PICTURE-ABI.md before using these declarations. */
#define ANYKA_PICTURE_VPSS_VERSION "libplat_vpss V1.3.05"
#define ANYKA_PICTURE_OSD_VERSION "libmpi_osd V1.0.12"
#define ANYKA_PICTURE_OSD_EX_VERSION "libapp_osd_ex V1.0.11"
#define ANYKA_PICTURE_EFFECT_ORIENTATION 8u
/* Scalar0..3: bit0 first ISP orientation word; bit1 second word.
 * Both bits3 = 180 degrees in original firmware's rotation call path. */
struct anyka_picture_orientation {uint32_t bit0,bit1;};
_Static_assert(sizeof(struct anyka_picture_orientation)==8,"target orientation ABI");
/* Original canvas creation consumes all eight words. Height precedes width.
 * alpha_percent conversion is observed; its accepted range needs verification.
 * Allocation uses DMA module17, which the current trial does not provision. */
struct anyka_picture_canvas {
    uint32_t channel,region,x,y,height,width,color_index,alpha_percent;
};
_Static_assert(sizeof(struct anyka_picture_canvas)==32,"target canvas ABI");
/* Sensor ISP profile: mode0 day,1 night; other0..4 labels are user profiles. */
typedef int (*anyka_picture_switch_mode_fn)(uint32_t device,uint32_t mode);
typedef int (*anyka_picture_effect_set_fn)(int device,uint32_t type,int value);
typedef int (*anyka_picture_effect_get_fn)(int device,uint32_t type,int *value);
typedef int (*anyka_picture_orientation_set_fn)(int device,const struct anyka_picture_orientation *);
typedef int (*anyka_picture_orientation_get_fn)(int device,struct anyka_picture_orientation *);
typedef int (*anyka_picture_osd_init_fn)(void); /* ak_osd_ex_init; not old SDK2arg API */
typedef int (*anyka_picture_osd_font_file_fn)(int file_font_size,const char *retained_path);
typedef int (*anyka_picture_osd_max_rect_fn)(uint32_t channel,int *width,int *height);
typedef int (*anyka_picture_osd_rect_fn)(int channel,int region,int x,int y,int width,int height);
typedef int (*anyka_picture_osd_font_size_fn)(uint32_t channel,int size);
typedef int (*anyka_picture_osd_draw_fn)(uint32_t channel,uint32_t region,uint32_t x,uint32_t y,
                                        const uint16_t *characters,int character_count);
typedef int (*anyka_picture_osd_clean_fn)(uint32_t channel,uint32_t region,uint32_t x,uint32_t y,
                                         int width,int height);
typedef int (*anyka_picture_osd_color_fn)(uint32_t foreground,uint32_t background);
/* Do not declare AE/AWB/MWB payloads from another SDK. Their byte layout and
 * units remain unresolved on this target. No raw-payload write API is exposed. */
#endif
