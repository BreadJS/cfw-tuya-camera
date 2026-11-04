/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_PICTURE_EXECUTOR_H
#define ANYKA_PICTURE_EXECUTOR_H
#include "board_control.h"
struct anyka_management;
struct anyka_executor;
struct anyka_audio;
enum anyka_board_action {ANYKA_BOARD_DAY=1,ANYKA_BOARD_IR,ANYKA_BOARD_LED,ANYKA_BOARD_MOVE,ANYKA_BOARD_STOP,ANYKA_BOARD_HOLD};
int anyka_executor_open(struct anyka_executor **,struct anyka_management *,struct anyka_board *,const char *font_path,int enable_osd);
/* Bounded synchronous command submission; SDK/sysfs/ioctls run on worker only.
 * MOVE is a <=64-step nudge; HOLD renews400ms smooth finite-travel lease.
 * Both take direction0..4; STOP ends either mode. */
int anyka_executor_board(struct anyka_executor *,enum anyka_board_action,int value);
/* Release the queued settings snapshot after initial board/profile setup. */
void anyka_executor_activate(struct anyka_executor *);
int anyka_executor_set_video_fps(struct anyka_executor *,unsigned fps);
int anyka_executor_set_encoder(struct anyka_executor *,unsigned handle);
void anyka_executor_set_audio(struct anyka_executor *,struct anyka_audio *);
int anyka_executor_status(struct anyka_executor *,struct anyka_board_status *);
/* Stop motors and acknowledge no further userspace SDK/control calls. Capture
 * resources remain retained; this does not stop DMA or free driver state. */
int anyka_executor_pause(struct anyka_executor *);
#endif
