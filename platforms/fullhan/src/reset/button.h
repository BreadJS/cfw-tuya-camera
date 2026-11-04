#ifndef CAMERA_RESET_BUTTON_H
#define CAMERA_RESET_BUTTON_H
#include <stdint.h>
#define CAMERA_RESET_HOLD_MS (UINT64_C(1000)*CAMERA_RESET_HOLD_SECONDS)
struct camera_reset_button {
    uint64_t last,changed,pressed_at;
    int candidate,stable;
    unsigned seen,armed,pressed;
};
void camera_reset_button_init(struct camera_reset_button *);
/* Returns1 only after a debounced release following a continuous five-second
 * hold. Requires release after startup/read errors/busy periods before arming.
 * level is the physical GPIO23 level (0pressed,1released), negative on errors.
 * Long sample gaps and clock regressions invalidate the gesture. */
int camera_reset_button_sample(struct camera_reset_button *,int level,uint64_t now_ms,int busy);
/* Fixed board input only. No output-level writes, no flash/config operations.
 * Returns a retained readonly value FD or a negative errno. */
int camera_reset_button_gpio_open(void);
int camera_reset_button_gpio_read(int fd);
#endif
