/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_VIDEO_MODE_H
#define CAMERA_ANYKA_VIDEO_MODE_H
#define ANYKA_VIDEO_PROFILE_MAIN 0u
#define ANYKA_VIDEO_PROFILE_BASELINE 2u
struct anyka_video_mode {unsigned width,height,fps,profile;};
/* Startup requests only: 1920x1080 or1280x720,15 or25fps, Main0/Baseline2.
 * Sensor crop and VI backing remain1920x1080; output geometry is a channel
 * choice. Static support does not assert measured delivered frame cadence. */
void anyka_video_mode_defaults(struct anyka_video_mode *);
int anyka_video_mode_validate(const struct anyka_video_mode *);
/* Private0700 owned directory FD; file is16-byte versioned0600/nlink1 record.
 * Missing file returns current1080p15/Main defaults. Malformed/untrusted state
 * returns an error without modifying *out; caller may explicitly choose safe
 * defaults while surfacing that error. No SDK/hardware operation occurs. */
int anyka_video_mode_load(int directory,struct anyka_video_mode *out);
/* Atomic durable save. Takes effect only after caller restarts the whole
 * pipeline. Error after rename may already be committed; caller re-reads. */
int anyka_video_mode_save(int directory,const struct anyka_video_mode *);
#endif
