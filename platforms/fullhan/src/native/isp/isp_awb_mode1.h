#ifndef CAMERA_ISP_AWB_MODE1_H
#define CAMERA_ISP_AWB_MODE1_H
#include "isp_awb.h"
/* Matching MIS2006 day profile bytes75..77 are all255. Inputs are the
 * completed, hardware chromaticity-filtered RGB sums normalized by512/ref.
 * No square-root or gamma transform belongs on these statistics. */
struct fh_awb_mode1_config {
    uint32_t minimum_mean,saturation_mean;
    unsigned minimum_cells;
    uint8_t spread_low,spread_middle,spread_high;
};
#define FH_AWB_MODE1_DAY_CONFIG {16,4090,3,255,255,255}
struct fh_awb_mode1_result {
    struct fh_awb_gains gains; /* normalizedG512, bounded256..2048 */
    struct fh_awb_gains pre_luma; /* exact mode1 blended gains, before normalization */
    unsigned accepted_cells,selected_cell,selected_weight,changed;
};
/* Pure recovered target calculation0x10b160/0x10aa8c, without its global
 * startup retry/filter-MMIO/writer behavior. Native dark/clipping/count guards
 * can hold rather than calling stock assertions.1=target,0=hold,negative=error.
 * selected_weight0..8 blends ranked-cell and aggregate inverse RGB gains.
 * Strict stock sorting/tie order and upper-middle selected index are retained.
 * Only pre_luma has exact integer parity: normalization/clamping are policy. */
int fh_awb_mode1_target(const struct fh_awb_mode1_config *,const struct fh_awb_sample *,struct fh_awb_mode1_result *);
/* Same fresh-generation, <=32Q9 andceil(distance/16) smoothing contract as
 * fh_awb_plan. Plan on state copy; commit only after coherent publication. */
int fh_awb_mode1_plan(const struct fh_awb_mode1_config *,struct fh_awb_state *,const struct fh_awb_sample *,struct fh_awb_mode1_result *);
#endif
