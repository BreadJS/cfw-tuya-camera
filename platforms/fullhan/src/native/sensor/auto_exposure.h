#ifndef CAMERHACK_AUTO_EXPOSURE_H
#define CAMERHACK_AUTO_EXPOSURE_H
#include <stdint.h>

/* Pure controller: no sensor, ISP, timers or implicit brightness-unit guess.
 * The caller supplies a fresh, source-normalized brightness sample and applies
 * the resulting pair through the verified sensor controls. */
struct fh_ae_config {
    uint32_t target_q16;
    uint32_t min_lines,max_lines,min_gain_q6,max_gain_q6;
    unsigned deadband_percent;
    /* Optional rational shutter quantum in sensor lines. Bothzero disables.
     * Eg675/2 lines is10ms at33750lines/s. No mains/line rate is inferred.
     * With quantization, shutter alignment wins over reaching the brightness
     * target when available gain is exhausted. */
    uint32_t shutter_quantum_num,shutter_quantum_den;
};
struct fh_ae_setting { uint32_t lines,gain_q6; };
/* Returns1 for a changed setting,0 for no change, negative errno on rejection.
 * Every accepted output stays inside caller bounds and sensor gain64..1024.
 * Change in total integration*gain is limited to approximately1.5x per observation
 * (integer setting rounding applies), except
 * when the current setting is already outside the configured allowed range.
 * Lowest gain is preferred subject to maximum shutter time. Output is unchanged
 * on rejection. No controller state is committed before hardware succeeds. */
int fh_ae_plan(const struct fh_ae_config *,const struct fh_ae_setting *,
               uint32_t brightness_q16,struct fh_ae_setting *);
#endif
