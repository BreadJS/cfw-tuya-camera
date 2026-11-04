/* SPDX-License-Identifier: MIT */
#ifndef ANYKA_BOARD_CONTROL_H
#define ANYKA_BOARD_CONTROL_H
#include <stdint.h>
struct anyka_board;
struct anyka_motor_status {int32_t busy,position,steps_per_second,degrees_per_second,steps_per_circle,total_steps,boundary_steps,speed_step;};
struct anyka_board_status {int day_mode,ir_led,status_led;int motors_available;struct anyka_motor_status pan,tilt;};
/* Exclusive motor ownership; no calibration or configuration ioctl is issued.
 * GPIOs are restricted to this board's exact pins and LED names. */
int anyka_board_open(struct anyka_board **,int enable_motors);
int anyka_board_status(struct anyka_board *,struct anyka_board_status *);
int anyka_board_day(struct anyka_board *,int day);
int anyka_board_ir(struct anyka_board *,int enabled);
int anyka_board_led(struct anyka_board *,int enabled);
/* Finite relative movement only, <=128steps, normal kernel boundaries.
 * direction0 stop,1left,2right,3up,4down. rotation reverses both axes. */
int anyka_board_move(struct anyka_board *,unsigned direction,unsigned steps,int rotated);
int anyka_board_stop(struct anyka_board *);
/* Smooth held movement on calibrated4096/2048,600steps/s axes only. Starts
 * one finite normal-boundary move; same-direction renewals only extend a
 * 400ms lease. Release/expiry stops; no cruise or boundary bypass ioctl. */
int anyka_board_hold(struct anyka_board *,unsigned direction,int rotated);
/* Optional exclusive-owner startup only. Accepts a stopped, already configured
 * handoff without moving. Otherwise only exact fresh driver defaults are homed.
 * Blocking kernel ioctl is not safely cancellable; see PTZ-CALIBRATION.md.
 * Any failure requires keeping this owner/fds alive and a cold recovery, not
 * retrying calibration or handing the hardware to another process. */
int anyka_board_calibrate(struct anyka_board *);
void anyka_board_tick(struct anyka_board *,uint64_t monotonic_ms);
void anyka_board_close(struct anyka_board *);
#endif
