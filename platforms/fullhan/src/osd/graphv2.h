#ifndef CAMERAHACK_OSD_GRAPHV2_H
#define CAMERAHACK_OSD_GRAPHV2_H
#include <stddef.h>
#include <stdint.h>
#define CAMERA_GRAPHV2_SET UINT32_C(0xc448696d)
#define CAMERA_GRAPHV2_WORDS 147u
struct camera_graphv2_bitmap {
    uint32_t physical,bytes,width,height,stride,x,y,slot,frame_width,frame_height;
};
/* Pure builder for the byte-matched camera's indexed2-bit channel plane.
 * It opens nothing and submits no ioctl. Bitmap packing/update lifetime must
 * be proven separately before this packet is used against hardware. */
int camera_graphv2_timestamp(const struct camera_graphv2_bitmap *,int enabled,
                              uint32_t wire[CAMERA_GRAPHV2_WORDS]);
#endif
