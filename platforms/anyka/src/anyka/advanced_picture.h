/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_ADVANCED_PICTURE_H
#define ANYKA_ADVANCED_PICTURE_H
#include <stddef.h>
struct anyka_advanced;
struct anyka_advanced_settings {
    int configured,auto_exposure,max_exposure_us,min_exposure_us,exposure_us;
    int gain_q8,min_gain_q8,max_gain_q8,auto_white_balance,wb_red_q8,wb_green_q8,wb_blue_q8;
    int temporal_nr,gamma_lift;
};
void anyka_advanced_defaults(struct anyka_advanced_settings *);
int anyka_advanced_valid(const struct anyka_advanced_settings *);
/* Exact shipped SDK only. OWNER WORKER ONLY: reads baseline using guarded
 * destination pages and proven kernel copy sizes. Opens no media devices.
 * Caller retains resources on error; no guessed SDK shutdown. */
int anyka_advanced_open(struct anyka_advanced **);
/* Read-current/modify-known-fields/set, keeping every unknown profile byte.
 * Gamma/TNR use retained baseline to avoid compounding repeated changes.
 * Call rebaseline immediately AFTER a successful day/night profile switch,
 * BEFORE reapplying user settings. No HTTP/source callback calls allowed. */
int anyka_advanced_rebaseline(struct anyka_advanced *);
/* Owner-only: cap AE and manual exposure at VTS-1 rows for 15/25fps. */
int anyka_advanced_limit_fps(struct anyka_advanced *,unsigned fps);
unsigned anyka_advanced_max_us(unsigned fps);
void anyka_advanced_clamp(struct anyka_advanced_settings *,unsigned fps);
int anyka_advanced_apply(struct anyka_advanced *,const struct anyka_advanced_settings *);
#endif
