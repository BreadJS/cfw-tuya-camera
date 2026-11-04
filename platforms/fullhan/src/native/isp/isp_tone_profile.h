#ifndef CAMERA_ISP_TONE_PROFILE_H
#define CAMERA_ISP_TONE_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#define FH_TONE_PROFILE_MAX_WRITES 44u
struct fh_tone_profile_write {uint32_t offset,keep_mask,value;};
struct fh_tone_profile_plan {
 struct fh_tone_profile_write writes[FH_TONE_PROFILE_MAX_WRITES];
 size_t count;
 unsigned curve_index,secondary_index;
};
/* Exact MIS2006 day profile260..2a7 gain64..1024Q6 lookup + original111a88
 * static curves. Pure: no reads/writes or enable-state assumption. Value is
 * merged as(old&keep_mask)|value. Includes24 bit18 original bypass setting,
 *23c lowbyte only (preserves adaptive shadow),254/258 and40curve words.
 * Caller may intentionally override24's bypass policy separately. Serialize
 * with adaptive shadow publication; no frame-atomic update is promised. */
int fh_tone_profile_make_plan(unsigned gain_q6,struct fh_tone_profile_plan *);
#endif
