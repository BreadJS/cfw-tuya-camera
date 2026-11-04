#ifndef CAMERA_COLOR_AUTO_H
#define CAMERA_COLOR_AUTO_H
#include "isp_awb.h"
#include "../platform/linux_io.h"
#include "color_profile.h"
struct fh_color_plan {uint32_t gain01,gain23,clip,ccm[6];unsigned first,second,weight;};
/* Pure exact-profile selector/interpolator + stock mode0 luma/BLC policy.
 * Gains logicalRGB Q9. MMIO configuration passed as values, never pointers. */
int fh_color_make_plan(const struct fh_awb_gains *,unsigned bayer,uint32_t blc0,uint32_t blc1,
                       uint32_t y0,uint32_t y1,struct fh_color_plan *);
/* Existing exclusive owner's mapped ISP only; no opens/allocations/restarts.
 * All colour writes serialized with illumination by the platform caller.
 * sticky fault after write failure; no false claim of atomic frame commit. */
int fh_color_publish(struct fh_linux_io *,const struct fh_color_plan *,int *fault);
/* Exact day mode1 neutral-colour statistics window, before producer startup. */
int fh_color_statistics_profile(struct fh_linux_io *);
#endif
