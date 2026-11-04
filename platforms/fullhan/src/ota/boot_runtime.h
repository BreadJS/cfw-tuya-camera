#ifndef CAMERA_BOOT_RUNTIME_H
#define CAMERA_BOOT_RUNTIME_H
#include "boot_confirm.h"

/* Pure strict parsers, also used by host tests. cmdline returns1 only for a
 * single installed NOR root and no trial/recovery/alternate-init arguments.
 * UUID parser accepts exactly36 ASCII bytes plus an optional final newline. */
int camera_boot_cmdline_is_installed(const char *,size_t);
int camera_boot_parse_id(const char *,size_t,uint8_t out[16]);

/* Read-only metadata:0 no journal (including a missing state directory),1 an
 * owned private240-byte install journal exists,-1 unsafe/inaccessible state.
 * Treat-1 as a reason to defer ordinary updater initialization, not as absence. */
int camera_boot_confirmation_needed(const char *state_path);

/* Threadless production adapter. Call before ordinary ota_state_open or
 * control_update_init. Locally proves installed NOR origin, RAM execution,
 * persistent JFFS2 config, pinned kernel/watchdog clock and read-only MTD
 * identity/geometry before calling boot_confirm. media_healthy must come from
 * real video/audio samples in this boot, never an HTTP input/version string.
 * Returns0 no journal,1 confirmed/recovered,-1 failure. On failure retry before
 * opening ordinary staging state: accepted may already have atomically moved.
 * No erase/write MTD ioctl, physical write FD, fork or worker is created here. */
int camera_boot_confirm_installed(const char *key_path,const char *state_path,
                                  int media_healthy,struct ota_boot_result *,
                                  char *error,size_t error_size);
#endif
