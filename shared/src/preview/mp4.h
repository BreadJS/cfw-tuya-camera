#ifndef CAMERA_PREVIEW_MP4_H
#define CAMERA_PREVIEW_MP4_H
#include "preview.h"
/* Pure bounded mux helpers, also used directly by offline fixture tests. */
int preview_mp4_init(const struct preview_config *,uint8_t *,size_t,size_t *);
int preview_mp4_fragment(const uint8_t *,size_t,int keyframe,uint64_t decode_time,
                         uint32_t duration,uint32_t sequence,uint8_t *,size_t,size_t *);
#endif
