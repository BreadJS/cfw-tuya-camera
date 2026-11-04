#ifndef CAMERA_IRCUT_H
#define CAMERA_IRCUT_H
#include <pthread.h>
#include <stdint.h>
#define IRCUT_PULSE_US CAMERA_IRCUT_PULSE_US
enum ircut_mode { IRCUT_UNKNOWN=-1, IRCUT_DAY=0, IRCUT_NIGHT=1 };
struct ircut_io {
    void *opaque;
    int (*check_owner)(void *);
    int (*prepare_pin)(void *,unsigned pin); /* only19/20; output initiallylow */
    int (*write_pin)(void *,unsigned pin,unsigned value);
    int (*sleep_us)(void *,unsigned microseconds);
};
struct ircut_status {
    enum ircut_mode commanded;
    unsigned prepared,idle_ack,faulted;
    int last_error,cleanup_error;
    uint64_t pulses;
};
struct ircut { pthread_mutex_t mutex;struct ircut_io io;unsigned flip;struct ircut_status status; };
/* Pure initialization, no GPIO access. Exact current product ircut_flip=1.
 * Own verified exclusive GPIO/control lifetime throughout every operation. */
int ircut_init(struct ircut *,const struct ircut_io *,unsigned flip);
/* Prepares both outputs low, acknowledges idle only; detent remains UNKNOWN.
 * Also explicit recovery after a fault; never reports physical position. */
int ircut_prepare_idle(struct ircut *);
/* Blocking300ms pulse; call only from control worker. Same successfulcommand
 * is a no-op, so LED-only changes must not repulse. DAY/NIGHT semanticpolarity
 * follows recovered flip. Idlecleanup attempted on failure, exacterrors kept.
 * No positionfeedback or hard OS scheduling deadline is claimed. Callerowns
 * repeat-change cooldown policy (suggested fewseconds), no autonomousauto mode. */
int ircut_set(struct ircut *,enum ircut_mode);
int ircut_status(struct ircut *,struct ircut_status *); /* trylock, -EBUSY */
struct ircut_linux {
    void *owner;int (*check)(void *);int value_fd[2];
};
/* Stores mandatory real owner check only; no hardware access. LinuxvalueFDs
 * remain retained after use/error. No unexport or claimed physicalreset. */
int ircut_linux_init(struct ircut_linux *,void *owner,int (*check)(void *));
struct ircut_io ircut_linux_io(struct ircut_linux *);
#endif
