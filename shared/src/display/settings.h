#ifndef CAMERA_DISPLAY_SETTINGS_H
#define CAMERA_DISPLAY_SETTINGS_H
#include <time.h>
struct camera_display_settings {
    char name[64];
    unsigned timestamp_enabled,timezone; /* 0 Europe/Amsterdam, 1 UTC */
};
void camera_display_defaults(struct camera_display_settings *);
/* Names contain 1..63 bytes of strict UTF-8, no ASCII controls or DEL. */
/* Return1 valid, 0 invalid. */
int camera_display_valid(const struct camera_display_settings *);
/* Directory FD must be private and owned by service UID. Absent load returns
 * defaults. Return 0 success, -1 unsafe/corrupt/I/O error; load preserves output
 * on error. Save is atomic and durable; an error after rename may be committed. */
int camera_display_load(int config_directory,struct camera_display_settings *);
int camera_display_save(int config_directory,const struct camera_display_settings *);
/* No process-global timezone mutation. Exactly23 bytes plus NUL on success.
 * Return0 success/-1 unsupported timezone, time conversion/year or NULL output. */
int camera_display_format_clock(time_t now,unsigned timezone,char out[24]);
#endif
