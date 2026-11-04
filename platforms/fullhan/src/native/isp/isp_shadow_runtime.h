#ifndef CAMERA_ISP_SHADOW_RUNTIME_H
#define CAMERA_ISP_SHADOW_RUNTIME_H
#include "isp_shadow.h"
#include "isp_stats.h"
#include "../platform/linux_io.h"
struct fh_shadow_runtime {struct fh_shadow_state state;int fault;};
/* Existing owner's mappings only. Caller holds illumination colour lock and
 * permits Day/IR-off. Invalid statistics hold state. Write failure is sticky;
 * no DMA resource is restarted/released and state commits only after success. */
int fh_shadow_update(struct fh_linux_io *,struct fh_shadow_runtime *,const struct fh_isp_stats_sample *,unsigned gain_q6);
int fh_shadow_update_config(struct fh_linux_io *,struct fh_shadow_runtime *,const struct fh_isp_stats_sample *,unsigned gain_q6,const struct fh_shadow_config *);
#endif
