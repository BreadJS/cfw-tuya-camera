#ifndef CAMERA_INDICATOR_CONTROLLER_H
#define CAMERA_INDICATOR_CONTROLLER_H
#include "status_led.h"
struct indicator {struct status_led hardware;unsigned red_mask,green_mask;};
/* Color mapping comes from the actual board observation, never from the
 * generic channel labels. Init is pure; prepare_off initializes safelevels. */
int indicator_init(struct indicator *,const struct status_led_io *,unsigned red_mask,unsigned green_mask);
int indicator_prepare_off(struct indicator *);
int indicator_apply(void *,unsigned mode); /*0off,1red,2green,3both*/
int indicator_status_json(void *,char *,size_t);
#endif
