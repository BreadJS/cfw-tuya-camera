#ifndef CAMERA_ROTATION_SETTINGS_H
#define CAMERA_ROTATION_SETTINGS_H
/* Private config directory FD. Missing file defaults to0. Save is atomic and
 * durable; an error after rename can be committed, so callers re-read it. */
int camera_rotation_load(int directory,unsigned *enabled);
int camera_rotation_save(int directory,unsigned enabled);
#endif
