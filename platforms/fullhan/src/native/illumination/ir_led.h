#ifndef CAMERA_IR_LED_H
#define CAMERA_IR_LED_H
#include <pthread.h>
struct ir_led_io {
    void *opaque;
    int (*prepare)(void *); /* GPIO3 only, direction out; no claim on level */
    int (*write)(void *,unsigned on); /* actual ir_led_ctrl=0: active high */
};
struct ir_led_status { int commanded;unsigned prepared,faulted;int last_error; };
struct ir_led {
    pthread_mutex_t mutex;
    void *owner;int (*check_owner)(void *);
    struct ir_led_io io;struct ir_led_status status;
};
/* Pure init. Current MIS2006 board only: GPIO3, ir_led_ctrl=0. Mandatory
 * caller-owned exclusive control lease; check returns0 only while valid.
 * No ISP monochrome/profile change, IR-cut pulse or automatic night policy. */
int ir_led_init(struct ir_led *,void *owner,int (*check)(void *),const struct ir_led_io *);
/* Explicit GPIO preparation and OFF command; can recover failed initialization.
 * direction/out may momentarily precede value/off. Call from control worker. */
int ir_led_prepare_off(struct ir_led *);
int ir_led_set(struct ir_led *,unsigned on);
/* trylock; commanded acknowledges sysfs write, NOT optical LED feedback. */
int ir_led_get(struct ir_led *,struct ir_led_status *);
struct ir_led_linux { void *owner;int (*check)(void *);int value_fd; };
/* Init performs no device access. Backend retains value fd through faults;
 * no unexport or false shutdown success. Caller owns backend lifetime. */
int ir_led_linux_init(struct ir_led_linux *,void *owner,int (*check)(void *));
struct ir_led_io ir_led_linux_io(struct ir_led_linux *);
#endif
