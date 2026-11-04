#ifndef CAMERA_ISP_AWB_H
#define CAMERA_ISP_AWB_H
#include <stdint.h>
#define FH_AWB_CELLS 9u
struct fh_awb_cell {uint32_t red,green,blue,count;};
struct fh_awb_sample {uint64_t generation;struct fh_awb_cell cell[FH_AWB_CELLS];};
struct fh_awb_gains {uint32_t red,green,blue;}; /* Logical RGB, Q9 (512unity). */
struct fh_awb_config {
    /* Caller-established statistics units: accepted channel means are
     * >=minimum_mean and <saturation_mean. No raw/display normalization or
     * ADC depth is guessed. SUM/COUNT and refs512 come from owner's contract. */
    uint32_t minimum_mean,saturation_mean;
    /* Calibrated broad neutral envelope for raw R/G and B/G, Q9 ratios.
     * These are explicit native policy, not recovered stock AWB thresholds. */
    uint32_t red_ratio_min,red_ratio_max,blue_ratio_min,blue_ratio_max;
    unsigned minimum_cells; /* 3..9 spatial cells required for robust consensus. */
};
struct fh_awb_state {
    uint64_t generation;
    struct fh_awb_gains gains;
    unsigned initialized;
};
struct fh_awb_result {
    struct fh_awb_gains gains;
    unsigned accepted_cells,changed;
};
/* Pure source-owned estimator. No statistics fetch, MMIO, sensor, profile,
 * Bayer mapping, CCM, allocation or clock calls. Gains are bounded256..2048.
 * Initialize from actual logical gains; green is normalized to512. */
int fh_awb_init(struct fh_awb_state *,const struct fh_awb_gains *initial);
/* 1=reliable estimate (inspect changed before publication),0=no reliable
 * neutral consensus: retain current gains,negative=invalid/stale. Fresh
 * generation is consumed even for unavailable samples. State/gains never
 * change on an error. Smoothing advances<=32Q9 and<=ceil(distance/16) per
 * accepted generation. Colored scenes can be fundamentally ambiguous with
 * nine RGB aggregates; confidence/envelope guards are not object recognition
 * or a claim of stock AWB parity. Caller owns fresh/completed-bank validity.
 * Owner should plan using a state copy, publish coherently, then commit that
 * state only after successful hardware publication (no automatic rollback). */
int fh_awb_plan(const struct fh_awb_config *,struct fh_awb_state *,
                const struct fh_awb_sample *,struct fh_awb_result *);
#endif
