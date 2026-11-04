#ifndef CAMERA_PICTURE_SETTINGS_H
#define CAMERA_PICTURE_SETTINGS_H
#include "../native/isp/picture_isp.h"
#include "../native/capture/fh_encoder_settings.h"
#include <stddef.h>
#define CAMERA_PICTURE_BYTES 256u
#define CAMERA_PICTURE_FORM_FIELDS 48u
struct camera_picture_settings {
 struct fh_picture_isp_config isp;
 unsigned auto_exposure,exposure_us,gain_q6,min_gain_q6,max_gain_q6;
 unsigned auto_white_balance,wb_red_q9,wb_green_q9,wb_blue_q9;
 unsigned gamma_enabled,gamma_profile;
 unsigned shadow_enabled,shadow_target,shadow_bias,shadow_low,shadow_middle,shadow_high;
 unsigned ccm_mode; /*0auto,1identity,2manual; nine row-major coefficients +3offsets. */
 int ccm[12];
 struct fh_encoder_settings encoder;
};
void camera_picture_defaults(struct camera_picture_settings *);
/*1 valid,0 invalid; includes encoder support and cross-field constraints. */
int camera_picture_valid(const struct camera_picture_settings *);
/* All following functions return0 on success,negative errno on failure.
 * Load absent file returns defaults. Directory/file owner+private mode+links
 * are checked. Save atomically renames+syncs; failure after rename can commit.
 * Invalid/corrupt input and format failures preserve caller output. */
int camera_picture_load(int directory,struct camera_picture_settings *);
int camera_picture_save(int directory,const struct camera_picture_settings *);
/* Exactly48known fields; booleans are numeric0/1. Reject missing/duplicate/
 * unknown fields, syntax errors, overflow and unsupported combinations. */
int camera_picture_parse_form(const char *,struct camera_picture_settings *);
/* Flat JSONobject using same fieldnames as the form; no outer wrapper. */
int camera_picture_json(const struct camera_picture_settings *,char *,size_t);
#endif
