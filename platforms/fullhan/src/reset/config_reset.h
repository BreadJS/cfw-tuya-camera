#ifndef CAMERA_CONFIG_RESET_H
#define CAMERA_CONFIG_RESET_H
#include <stddef.h>
#define CAMERA_CONFIG_RESET_MARKER "reset-request"
typedef int (*camera_reset_fault)(void *,const char *);
/* Caller owns a private config-directory FD and serializes all settings writers.
 * install_busy must come from local installer state. Caller holds its updater
 * mutex through request and permanently gates mutating APIs after success until
 * reboot. ota_lock_fd is a borrowed same-description lock, or -1 to acquire a
 * local nonblocking lock. Borrowed descriptors remain open and locked.
 * No files are reset by request: only a durable marker is published. */
int camera_config_reset_request(int config_fd,int ota_lock_fd,int install_busy,
    camera_reset_fault,void *,char *error,size_t error_size);
/* Earlyboot only, before any settings/network/media writers. Return 0 no marker,
 * 1 reset completed, -1 failure (boot must stop). Preserves all OTA/factory data.
 * A durable marker remains until all six own files and strict own temporaries
 * have been removed and their directory fsynced. No GPIO, reboot or MTD calls. */
int camera_config_reset_apply(int config_fd,int ota_lock_fd,
    camera_reset_fault,void *,char *error,size_t error_size);
/* Define CAMERA_CONFIG_RESET_MAIN to build the production --apply CLI. The CLI
 * additionally requires root, RAM root, NOR origin and JFFS2 config31:5. */
#endif
