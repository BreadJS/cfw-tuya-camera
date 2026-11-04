#ifndef CAMERA_ILLUMINATION_CONTROLLER_H
#define CAMERA_ILLUMINATION_CONTROLLER_H
#include "ircut.h"
#include "ir_led.h"
#include "../platform/linux_io.h"
struct illumination_color_io {
    void *opaque;
    int (*read)(void *,uint32_t,uint32_t *);
    int (*write)(void *,uint32_t,uint32_t);
    int (*delay_us)(void *,uint32_t);
};
struct illumination {
    pthread_mutex_t mutex;
    struct ircut cut;
    struct ir_led led;
    struct illumination_color_io color;
    unsigned day_saturation,prepared,night,monochrome_known,busy;
    int last_error;
    uint64_t last_pulse_ms;
};
/* Caller initializes exact board GPIO adapters, supplies already owned ISP
 * register callbacks after the daylight seed. Prepare drives LEDoff+coilsidle
 * and captures only the daylight saturation field; no detent inference. */
int illumination_init(struct illumination *,const struct ircut_io *,
    void *owner,int (*check)(void *),const struct ir_led_io *,
    const struct illumination_color_io *);
int illumination_prepare(struct illumination *);
/* Called from the separate HTTP/control worker. Independent manual LED state;
 * filter changes suppress illumination during the pulse, then set requestedLED.
 * 3s minimum completed-pulse interval is application policy, not a rated limit.
 * No automatic switcher, ADC/lux inference or fake position feedback. */
int illumination_apply(void *,unsigned night,unsigned ir_on);
/* 1=daylight colour update lease held; 0=night/unknown/busy; negative=error. */
int illumination_color_begin(struct illumination *);
/* Generic image-control lease, also permits Night/IR. Caller keeps chroma0
 * for Night and updates day_saturation while holding this lock. */
int illumination_picture_begin(struct illumination *,unsigned *night);
void illumination_color_end(struct illumination *);
int illumination_status_json(void *,char *,size_t);
#endif
