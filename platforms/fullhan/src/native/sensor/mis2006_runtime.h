/* Original source-owned API: deliberately NOT the vendor callback ABI. */
#ifndef CAMERHACK_MIS2006_RUNTIME_H
#define CAMERHACK_MIS2006_RUNTIME_H
#include "mis2006_contract.h"

/* All callbacks return zero or a negative error. No positive/short success.
 * acquire must establish exclusive sensor/MIPI/ISP/encoder ownership. Caller
 * serializes all API calls. Failed prepare/open/configure can have side effects:
 * their paired cleanup MUST be safe even after a failed attempt, and retryable.
 * A failed acquire must leave no ownership or hardware side effects.
 * read performs one combined address-write/read transaction on the opened bus.
 * bus_open accepts a 7-bit address; no device number or clock is assumed here.
 * prepare/configure implement platform-verified lifecycle only; absent callbacks
 * fail with -ENOTSUP. No vendor control-block or MIPI defaults are supplied.
 * mipi_disable/unprepare must establish safe hardware quiescence before lease
 * release; merely unmapping registers does not fulfill this contract. Return
 * -ENOTSUP when safe shutdown is unknown, retaining ownership for recovery.
 */
struct mis2006_io {
    int (*acquire)(void *);
    int (*release)(void *);
    int (*prepare)(void *);
    int (*unprepare)(void *);
    int (*bus_open)(void *, uint8_t addr7);
    int (*bus_close)(void *);
    int (*write)(void *, const uint8_t wire[3]);
    int (*read)(void *, const uint8_t address[2], uint8_t *value);
    int (*delay_us)(void *, uint32_t);
    int (*mipi_configure)(void *, uint32_t format,
                          const struct mis2006_stock_format *);
    int (*mipi_disable)(void *);
};
enum mis2006_state { MIS2006_IDLE, MIS2006_ACTIVE, MIS2006_FAULT };
struct mis2006_runtime {
    struct mis2006_io io;
    void *opaque;
    enum mis2006_state state;
    unsigned owned, prepared, bus, mipi;
    uint8_t addr7;
    uint32_t format, exposure, gain_q6;
    int last_cleanup_error;
};
/* init requires fresh storage or a successfully stopped instance. */
int mis2006_runtime_init(struct mis2006_runtime *, const struct mis2006_io *, void *);
int mis2006_start(struct mis2006_runtime *, uint32_t format);
/* stop retries cleanup; ownership is retained while any cleanup fails. */
int mis2006_stop(struct mis2006_runtime *);
int mis2006_set_mode(struct mis2006_runtime *, uint32_t format);
int mis2006_set_exposure(struct mis2006_runtime *, uint32_t lines);
int mis2006_set_gain(struct mis2006_runtime *, uint32_t q6);
/* get_gain has the stock 0x3802-read / 0x3604-write side effect. */
int mis2006_get_gain(struct mis2006_runtime *, uint32_t *q6);
int mis2006_set_orientation(struct mis2006_runtime *, uint32_t bits);
/* Display-relative0/180degrees, including crop phase and five-register ACK.
 * Same sensor owner/thread as AE. Failure retains resources, marks FAULT. */
int mis2006_set_rotation(struct mis2006_runtime *, unsigned rotate_180);
int mis2006_get_orientation(struct mis2006_runtime *, unsigned *bit1);
int mis2006_read_register(struct mis2006_runtime *, uint16_t, uint8_t *);
/* Raw writes invalidate cached mode/control assumptions: instance becomes FAULT
 * after a successful raw write and must be stopped before reuse. */
int mis2006_write_register(struct mis2006_runtime *, uint16_t, uint8_t);
#endif
