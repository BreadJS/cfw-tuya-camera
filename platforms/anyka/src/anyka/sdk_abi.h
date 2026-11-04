/* SPDX-License-Identifier: MIT
 * SDK ABI declarations reconstructed from the owner's executable and compared
 * with medevil/ipcd's MIT cap_pipeline.c. See docs/third-party/ANYKA-ABI.md.
 * This file contains no original vendor implementation code.
 */
#ifndef CAMERA_ANYKA_SDK_ABI_H
#define CAMERA_ANYKA_SDK_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Fixed ARM EABI32 layouts. Neither host pointer size nor host unsigned-long
 * size may change the structures written by the original ARM SDK. Geometry,
 * enum semantics and stream timestamp units still require target validation.
 */
struct anyka_encode_param {
    uint16_t width;                 /* 0x00 */
    uint16_t height;                /* 0x02 */
    uint16_t fps;                   /* 0x04 */
    uint16_t gop_frames;            /* 0x06 */
    uint16_t target_kbps;           /* 0x08 */
    uint16_t maximum_kbps;          /* 0x0a */
    int32_t profile;                /* 0x0c: candidate H264 main=0 */
    int32_t rate_control;           /* 0x10: SDK enum, not stock INI enum */
    uint16_t initial_qp;            /* 0x14 */
    uint16_t minimum_qp;            /* 0x16 */
    uint16_t maximum_qp;            /* 0x18 */
    uint16_t reserved_1a;
    int32_t jpeg_quality;           /* 0x1c */
    int32_t chroma_mode;            /* 0x20 */
    int32_t codec;                  /* 0x24 */
    int32_t reserved_28;
    uint16_t encoder_level;         /* 0x2c */
    uint16_t reserved_2e;
    int32_t smart_mode;             /* 0x30 */
    uint16_t smart_gop_frames;      /* 0x34 */
    uint16_t smart_quality;         /* 0x36 */
    uint16_t smart_static_value;    /* 0x38 */
    uint16_t reserved_3a;
};

struct anyka_vi_device_attr {
    uint32_t device_id;             /* 0x00 */
    uint32_t reserved[4];           /* 0x04..0x10 */
    int32_t crop_x;                 /* 0x14 */
    int32_t crop_y;                 /* 0x18 */
    int32_t crop_width;             /* 0x1c */
    int32_t crop_height;            /* 0x20 */
    uint32_t main_max_width;        /* 0x24 */
    uint32_t main_max_height;       /* 0x28 */
    int32_t fps;                    /* 0x2c */
    uint32_t sub_max_width;         /* 0x30 */
    uint32_t sub_max_height;        /* 0x34 */
};

struct anyka_vi_channel_attr {
    uint32_t channel_id;
    int32_t fps;
    uint32_t width;
    uint32_t height;
    uint32_t frame_depth;
};

struct anyka_channel { int32_t module; int32_t channel; };
struct anyka_channel_attr { int32_t fps; int32_t frame_depth; };

/* SDK-owned storage. Copy the complete AU before release; never retain this
 * address after the corresponding release call. Timestamp units are UNPROVEN.
 */
struct anyka_video_stream {
    uint32_t data_address;
    uint32_t bytes;
    uint64_t source_timestamp;
    uint32_t sequence;
    int32_t frame_type;
};

#define ANYKA_DMA_MODULE_VI 7
#define ANYKA_DMA_MODULE_VENC 13

#define ANYKA_ABI_ASSERT(type, member, offset) \
    _Static_assert(offsetof(struct type, member) == (offset), #type "." #member " offset")
_Static_assert(sizeof(struct anyka_encode_param) == 60, "Anyka encoder ABI60");
ANYKA_ABI_ASSERT(anyka_encode_param, width, 0x00);
ANYKA_ABI_ASSERT(anyka_encode_param, target_kbps, 0x08);
ANYKA_ABI_ASSERT(anyka_encode_param, profile, 0x0c);
ANYKA_ABI_ASSERT(anyka_encode_param, rate_control, 0x10);
ANYKA_ABI_ASSERT(anyka_encode_param, initial_qp, 0x14);
ANYKA_ABI_ASSERT(anyka_encode_param, jpeg_quality, 0x1c);
ANYKA_ABI_ASSERT(anyka_encode_param, chroma_mode, 0x20);
ANYKA_ABI_ASSERT(anyka_encode_param, codec, 0x24);
ANYKA_ABI_ASSERT(anyka_encode_param, encoder_level, 0x2c);
ANYKA_ABI_ASSERT(anyka_encode_param, smart_mode, 0x30);
ANYKA_ABI_ASSERT(anyka_encode_param, smart_static_value, 0x38);
_Static_assert(sizeof(struct anyka_vi_device_attr) == 56, "Anyka VI device ABI56");
ANYKA_ABI_ASSERT(anyka_vi_device_attr, crop_x, 0x14);
ANYKA_ABI_ASSERT(anyka_vi_device_attr, main_max_width, 0x24);
ANYKA_ABI_ASSERT(anyka_vi_device_attr, fps, 0x2c);
ANYKA_ABI_ASSERT(anyka_vi_device_attr, sub_max_width, 0x30);
_Static_assert(sizeof(struct anyka_vi_channel_attr) == 20, "Anyka VI channel ABI20");
ANYKA_ABI_ASSERT(anyka_vi_channel_attr, frame_depth, 16);
_Static_assert(sizeof(struct anyka_channel) == 8, "Anyka module/channel ABI8");
_Static_assert(sizeof(struct anyka_channel_attr) == 8, "Anyka binding ABI8");
_Static_assert(sizeof(struct anyka_video_stream) == 24, "Anyka stream ABI24");
ANYKA_ABI_ASSERT(anyka_video_stream, bytes, 4);
ANYKA_ABI_ASSERT(anyka_video_stream, source_timestamp, 8);
ANYKA_ABI_ASSERT(anyka_video_stream, sequence, 16);
ANYKA_ABI_ASSERT(anyka_video_stream, frame_type, 20);
#undef ANYKA_ABI_ASSERT

#endif
