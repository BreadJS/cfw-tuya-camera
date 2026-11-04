/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_RESET_BUTTON_H
#define ANYKA_RESET_BUTTON_H
/* EvdevGPIO5/KEY_POWER116: five-second hold, reset only on release.
 * Callback executes on dedicated thread. No GPIO export, vendor callback or
 * settings deletion happens in this reader. */
int anyka_reset_start(void (*callback)(void *),void *opaque);
/* Removes only own known settings, preserving OTA sequence/journals and all
 * factory settings outside this private directory. Caller reboots afterward. */
int anyka_reset_settings(int private_directory);
#endif
