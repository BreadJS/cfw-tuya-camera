#ifndef CAMERA_ISP_SHADOW_H
#define CAMERA_ISP_SHADOW_H
#include <stdint.h>
struct fh_shadow_tile {uint32_t sum[4],count;};
/* Original1111a4 reducer. Caller supplies exactly(row+1)*(col+1)-1 tiles
 * from coherent completed-bank Bayer statistics. Count is firstplaneCOUNT.
 * Preserves original initial min4095, mean truncation and log approximation. */
int fh_shadow_extrema(const struct fh_shadow_tile *,unsigned count,uint32_t *min_log_q8,uint32_t *max_log_q8);
struct fh_shadow_config {uint8_t ae_target,shadow_bias,low_scale,middle_scale,high_scale;};
#define FH_SHADOW_DAY_CONFIG {106,10,64,64,255}
struct fh_shadow_state {
 uint64_t generation;
 uint32_t previous_min,previous_max,smooth_min,smooth_max,hist0,hist2,mean_mass;
};
struct fh_shadow_input {
 uint64_t generation;
 uint32_t width,height,histogram[3],min_log_q8,max_log_q8;
 uint32_t reg23c,reg240,reg244,reg248,reg24c,reg250;
};
struct fh_shadow_plan {uint32_t reg23c,reg240,reg244,reg248,reg24c,reg250;};
/* Pure original1112ac adaptive endpoint/slope/shadow plan. State starts zero.
 * Freshness and plausible-input guards are native policy. No MMIO, statistics
 * ownership, tone-table enable bits, curve writes or publication is performed.
 * Plan on copied state and commit only after successful coherent publication.
 * Width/height matchISP30 low/high11bits, not arbitrary sensor dimensions.
 * Caller must separately establish whether adaptive hardware block is enabled.
 */
int fh_shadow_make_plan(const struct fh_shadow_config *,struct fh_shadow_state *,const struct fh_shadow_input *,struct fh_shadow_plan *);
#endif
