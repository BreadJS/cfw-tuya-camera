#ifndef CAMERA_GAMMA_CONTROL_H
#define CAMERA_GAMMA_CONTROL_H
#include "../isp_startup.h"
/* Existing retained ISP owner only. Copies the exact 652-byte driver packet;
 * no media/VMM opens, allocation, teardown or producer restart. IRQ applies it.
 * Check enable bits after frames have run; LUT SRAM readback is not reliable. */
int camera_gamma_submit(struct fh_linux_io *,int,const struct fh_isp_control_io *);
int camera_gamma_submit_profile(struct fh_linux_io *,int,const struct fh_isp_control_io *,unsigned profile,unsigned enabled);
int camera_gamma_verify(struct fh_linux_io *);
int camera_gamma_verify_enabled(struct fh_linux_io *,unsigned enabled);
#endif
