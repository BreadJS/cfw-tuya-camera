/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_ADVANCED_PICTURE_ABI_H
#define ANYKA_ADVANCED_PICTURE_ABI_H
#include <stddef.h>
#include <stdint.h>
/* Exact ak_isp.ko123c50... getter/copy_to_user sizes. These are target ABI
 * observations, not copied vendor implementation. Unknown bytes are retained. */
#define ANYKA_SENSOR_LINES_PER_SECOND 28125u
#define ANYKA_SENSOR_MAX_EXPOSURE_LINES 1874u /* fixed15fps MIS2006 mode */
struct anyka_advanced_ae {
    uint32_t exp_time_max,exp_time_min,d_gain_max,d_gain_min,isp_d_gain_min,isp_d_gain_max;
    uint32_t a_gain_max,a_gain_min,exp_step,exp_stable_range;
    uint32_t target_luminance[9],luminance_filter_percent;
    uint8_t reserved_profile_fields[72];
    uint32_t unknown_152,convergence_rate;
    uint32_t histogram_weights[16];
    uint8_t reserved_tail[16];
};
struct anyka_advanced_me {uint32_t exposure_lines,analog_gain_q8,digital_gain_q8,isp_gain_q8;};
struct anyka_advanced_mwb {uint16_t red_q8,green_q8,blue_q8;int16_t red_offset,green_offset,blue_offset;};
struct anyka_advanced_rgb_gamma {uint16_t red[129],green[129],blue[129],red_keys[16],green_keys[16],blue_keys[16],enabled;};
struct anyka_advanced_nr {uint16_t mode,padding;uint8_t profile[10][372];};
_Static_assert(sizeof(struct anyka_advanced_ae)==240,"AE240");
_Static_assert(offsetof(struct anyka_advanced_ae,convergence_rate)==156,"AE rate156");
_Static_assert(sizeof(struct anyka_advanced_me)==16,"ME16");
_Static_assert(sizeof(struct anyka_advanced_mwb)==12,"MWB12");
_Static_assert(sizeof(struct anyka_advanced_rgb_gamma)==872,"RGBgamma872");
_Static_assert(sizeof(struct anyka_advanced_nr)==3724,"NR3724");
#endif
