#ifndef CAMERA_OTA_LINUX_MTD_H
#define CAMERA_OTA_LINUX_MTD_H
#include "ota_apply_plan.h"
#include <sys/types.h>

enum ota_mtd_phase { OTA_MTD_OPEN, OTA_MTD_GEOMETRY, OTA_MTD_ERASE,
                     OTA_MTD_WRITE, OTA_MTD_READ, OTA_MTD_SYNC };
enum ota_mtd_sync {
    OTA_MTD_SYNC_FSYNC = 1,
    /* Explicit caller attestation of Linux 4.9 mtdchar release -> mtd_sync
     * and synchronous NOR erase/write semantics. No automatic fallback. */
    OTA_MTD_SYNC_RELEASE49 = 2
};
struct ota_mtd_lease {
    void *context;
    /* Mandatory trusted integration code, not a user-supplied boolean:
     * verify a still-held exclusive isolation lease bound to this exact plan.
     * Must establish RAM-resident helper/code/data, detached rootfs, stopped
     * readers/writers, controlled watchdog, trusted mounts/device topology and
     * the selected driver's synchronization semantics. Must also exclude
     * fork/dup/FD sharing so a separately opened sync FD has one reference.
     * Zero means valid. */
    int (*check)(void *context, const struct ota_apply_plan *, enum ota_mtd_phase,
                 enum ota_mtd_sync);
};
/* Syscall seam for host tests. NULL selects real Linux syscalls. A supplied
 * table is trusted code and must be complete. It cannot weaken production
 * policy: fixed paths, device numbers, geometry and bounds remain enforced. */
struct ota_mtd_syscalls {
    int (*open)(void *, const char *, int);
    int (*close)(void *, int);
    int (*fstat)(void *, int, struct stat *);
    int (*lstat)(void *, const char *, struct stat *);
    ssize_t (*read)(void *, int, void *, size_t);
    ssize_t (*pread)(void *, int, void *, size_t, off_t);
    ssize_t (*pwrite)(void *, int, const void *, size_t, off_t);
    int (*ioctl)(void *, int, unsigned long, void *);
    int (*fsync)(void *, int);
    int (*map)(void *, struct ota_mtd_map *, char *, size_t);
};
struct ota_linux_mtd {
    struct ota_apply_plan *plan;
    struct ota_mtd_lease lease;
    const struct ota_mtd_syscalls *sys;
    void *sys_context;
    enum ota_mtd_sync sync_mode;
    int fd[2], sync_fd[2];
    struct stat identity[2];
    unsigned selected, dirty, faulted, active;
    unsigned next_block, in_block, reading;
    uint32_t written, read_back;
};
/* Library only: no caller in any physical-apply CLI/service. Opening with real
 * syscalls requires a valid isolation lease BEFORE any device is opened. */
int ota_linux_mtd_open(struct ota_linux_mtd *, struct ota_apply_plan *,
                       const struct ota_mtd_lease *, enum ota_mtd_sync,
                       const struct ota_mtd_syscalls *, void *sys_context,
                       struct ota_apply_backend *, char *error, size_t);
/* Required after successful executor return. Synchronizes and closes all FDs,
 * reports close/fsync errors, and consumes the backend. No sequence commit.
 * Abort cleanup also closes all FDs, even when isolation has been revoked. */
int ota_linux_mtd_finish(struct ota_linux_mtd *, char *error, size_t);
int ota_linux_mtd_close(struct ota_linux_mtd *);
#endif
