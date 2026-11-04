#ifndef CAMERA_STATUS_LED_H
#define CAMERA_STATUS_LED_H
#include <pthread.h>
#define STATUS_LED_CHANNEL_A 1U /* exact GPIO2; color not yet verified */
#define STATUS_LED_CHANNEL_B 2U /* exact GPIO6; color not yet verified */
struct status_led_io {
    void *opaque;int(*check_owner)(void *);
    int(*prepare_pin)(void *,unsigned pin); /* pinctrl+export+outputhigh, fixed2/6 */
    int(*write_pin)(void *,unsigned pin,unsigned level);
    int(*read_pin)(void *,unsigned pin,unsigned *level);
};
struct status_led_state {
    unsigned prepared,commanded_known,on_mask,levels_known,high_mask;
    int last_error,cleanup_error;
};
struct status_led {pthread_mutex_t mutex;struct status_led_io io;struct status_led_state state;};
/* Pureinit. ChannelA=GPIO2/B=GPIO6; physical red/blue labels belong to caller's
 * actual-board validation. Stock controls use active-low levels. No IRpins. */
int status_led_init(struct status_led *,const struct status_led_io *);
/* Enables selectedchannels(mask0..3); zero=off,both=3. Single serializedcontrol
 * worker; failure reports unknowncommand and best-effort actualreadback. */
int status_led_set(struct status_led *,unsigned on_mask);
/* Read GPIO2/6 if prepared; no initialization/outputchanges. trylock -EBUSY. */
int status_led_get(struct status_led *,struct status_led_state *);
struct status_led_linux {void*owner;int(*check)(void *);int value_fd[2];};
int status_led_linux_init(struct status_led_linux *,void *,int(*check)(void *));
struct status_led_io status_led_linux_io(struct status_led_linux *);
#endif
