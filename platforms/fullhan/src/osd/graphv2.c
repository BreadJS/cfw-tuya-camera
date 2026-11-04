#include "graphv2.h"
#include "../native/vmm/legacy_vmm.h"
#include <errno.h>
#include <string.h>
_Static_assert(CAMERA_GRAPHV2_WORDS*4u==0x24cu,"matching driver copies0x24c, despite request size0x448");
int camera_graphv2_timestamp(const struct camera_graphv2_bitmap *b,int enabled,
                              uint32_t wire[CAMERA_GRAPHV2_WORDS])
{
    if (!b || !wire || (enabled!=0 && enabled!=1) || b->slot>3) return -EINVAL;
    if (enabled && (!b->width || !b->height || b->width>4096 || b->height>4096 ||
        ((b->x|b->y|b->width|b->height)&1u) || b->stride>65535 || (b->stride&7u) ||
        b->stride<(b->width+3u)/4u || b->physical<LEGACY_VMM_ZONE_START ||
        b->physical>=LEGACY_VMM_ZONE_END || b->bytes>LEGACY_VMM_ZONE_END-b->physical ||
        (uint64_t)b->stride*b->height>b->bytes ||
        (uint64_t)b->x+b->width>b->frame_width || (uint64_t)b->y+b->height>b->frame_height)) return -ERANGE;
    uint32_t words[CAMERA_GRAPHV2_WORDS]={0};
    words[0]=1; /* exact wrapper channel0 +1, downstream main-channel plane */
    words[1]=b->slot;words[2]=(uint32_t)enabled;
    if (enabled) {
        words[3]=2; /* indexed2-bit, as stock text layer */
        words[4]=b->physical;
        words[9]=2; /* matching stock GraphV2 mode */
        words[13]=b->width;words[14]=b->height;
        words[15]=b->x;words[16]=b->y;words[17]=b->stride;
        words[18]=0; /* color bank index; NOT palette[0] */
        words[19]=0;words[20]=UINT32_C(0xff000000);words[21]=UINT32_C(0xffffffff);words[22]=0;
    }
    memcpy(wire,words,sizeof words);return 0;
}
