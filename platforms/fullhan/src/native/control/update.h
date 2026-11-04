#ifndef CAMERA_CONTROL_UPDATE_H
#define CAMERA_CONTROL_UPDATE_H
#include "http.h"
#include "../../ota/ota_state.h"
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
struct control_update {
    struct ota_state state;
    pthread_mutex_t mutex;
    unsigned initialized;
    /* Status-only proof. Never authorizes physical apply or skips apply-time
     * verification. Retained FD prevents inode reuse while cached. */
    int cache_fd;
    unsigned cached;
    struct stat cache_identity;
    uint64_t cache_accepted;
    uint8_t cache_header[OTA_HEADER_SIZE];
    /* Install monitor owns waitpid; service context remains alive until reset. */
    pid_t install_pid;
    int progress_directory,install_failed,install_written,monitor_active,install_pristine;
    unsigned reset_pending; /* Set under mutex after physical reset request. */
    uint64_t install_sequence;
    uint8_t install_header[OTA_HEADER_SIZE];
};
/* Trusted root-owned key, persistent JFFS2 state directory, RAM staging only.
 * initialize_missing==1 explicitly permits baseline1 creation ONLY when no
 * accepted/pending/staged state exists. Never resets an existing baseline.
 * Caller configures authenticated HTTP; no key generation or network service.
 * Context must outlive the HTTP worker. */
int control_update_init(struct control_update *,const char *public_key,
                         const char *state_dir,const char *stage_dir,
                         int initialize_missing);
void control_update_get_status(struct control_update *,struct control_reply *);
void control_update_upload(struct control_update *,const struct control_body *,
                            struct control_reply *);
/* Discard only the fixed RAM capsule and signed pending journal. Never changes
 * the accepted sequence or flash. An interrupted discard can be retried. */
void control_update_cancel(struct control_update *,struct control_reply *);
void control_update_plan(struct control_update *,struct control_reply *);
void control_update_install(struct control_update *,uint64_t expected_sequence,
                            const char *expected_payload_digest,struct control_reply *);
int control_update_busy(struct control_update *);
/* Internal helpers: caller already holds this context's mutex. */
int control_update_can_install_locked(struct control_update *);
void control_update_install_status_locked(struct control_update *,char *,size_t);
int control_update_discard_install_locked(struct control_update *);
/* Only after the service worker has stopped using this context. */
int control_update_close(struct control_update *);
/* Production apply runs in the fixed RAM installer through retained trusted
 * descriptors. It rechecks detached RAM userspace, hardware watchdog pause,
 * exact kernel/MTD layout and signed bytes before erase/write/readback. SD RAM
 * trials cannot enable web installation. Accepted advances only after another
 * NOR boot and authenticated installed-byte/media-health confirmation.
 * Single-bank flash has no automatic power-loss rollback. */
#endif
