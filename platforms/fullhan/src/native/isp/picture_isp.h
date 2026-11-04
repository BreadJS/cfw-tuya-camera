#ifndef CAMERA_PICTURE_ISP_H
#define CAMERA_PICTURE_ISP_H
#include "../platform/linux_io.h"
#include <stddef.h>
struct fh_picture_isp_config {
 unsigned saturation; /* ISP5d0 raw0..1020; current Day default320. */
 unsigned contrast; /* Original byte0..255; default60. */
 int luma_offset; /* Original signed byte-128..127, separate from AE target. */
 int hue_degrees; /* -180..180, exact original Q7 sine/cosine table. */
 unsigned cb_gain,cr_gain; /* Original chroma coefficients0..200; defaults54/50. */
 unsigned temporal_nr; /*0bypass,1enable; retained queried-mode1 backing required. */
 unsigned detail_percent; /* Native scaling of startup20/16/16 coefficients0..200. */
};
#define FH_PICTURE_ISP_WRITES 10u
struct fh_picture_isp_write {uint32_t offset,keep_mask,value;};
struct fh_picture_isp_plan {struct fh_picture_isp_write writes[FH_PICTURE_ISP_WRITES];size_t count;};
void fh_picture_isp_defaults(struct fh_picture_isp_config *);
int fh_picture_isp_valid(const struct fh_picture_isp_config *);
int fh_picture_isp_make_plan(const struct fh_picture_isp_config *,struct fh_picture_isp_plan *);
/* Existing retained owner/mapping only; caller serializes with illumination,
 * colour/shadow updates. Night must supply saturation0 to retain grayscale.
 * Readback checks touched fields only. Any I/O failure is sticky. No opens,
 * allocations, gamma/LUT operations, producer changes or DMA release. */
int fh_picture_isp_publish(struct fh_linux_io *,const struct fh_picture_isp_plan *,int *fault);
#endif
