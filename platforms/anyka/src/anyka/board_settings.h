/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_BOARD_SETTINGS_H
#define ANYKA_BOARD_SETTINGS_H
struct anyka_board_settings {unsigned day_mode,ir_led,status_led;};
void anyka_board_settings_defaults(struct anyka_board_settings *);
/* Private owned0700 directory, regular0600 one-link settings. Missing returns
 * day1/IR0/LED0 without writing. Corrupt/unsafe files fail without defaults.
 * Save is atomic+durable; fsync error after rename can already be committed,
 * so callers re-read on error. No devices/SDK are touched. */
int anyka_board_settings_load(int directory,struct anyka_board_settings *);
int anyka_board_settings_save(int directory,const struct anyka_board_settings *);
#endif
