#ifndef CAMERA_WATCHDOG_CLOCK_PROBE_H
#define CAMERA_WATCHDOG_CLOCK_PROBE_H
#include <stddef.h>
#include <stdint.h>

struct camera_watchdog_clock_report {
    uint32_t first_value,second_value;
    unsigned fingerprint_valid,mapping_valid,read_complete;
    unsigned stable,clock_paused,driver_registered,cleanup_error;
};
/* Fixed-address, read-only check for the pinned stock/patch-compatible kernel.
 * Zero requires exact code/mapping fingerprints, two stable paused readings,
 * and an absent watchdog misc driver. Any unknown/error returns nonzero.
 * No fork/exec, register writes, watchdog opens, MTD opens or FD duplication.
 * This snapshot does not grant exclusivity or an installation lease. */
int camera_watchdog_clock_inspect(struct camera_watchdog_clock_report *,
                                  char *error,size_t error_size);
#endif
