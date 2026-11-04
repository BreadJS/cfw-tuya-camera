#ifndef CAMERA_OTA_BOOT_CONFIRM_H
#define CAMERA_OTA_BOOT_CONFIRM_H
#include "ota_apply_plan.h"
#include <sys/types.h>

#define OTA_BOOT_JOURNAL_HEADER_OFFSET 32U
#define OTA_BOOT_JOURNAL_RECORDS_OFFSET (OTA_BOOT_JOURNAL_HEADER_OFFSET + OTA_HEADER_SIZE)
#define OTA_BOOT_JOURNAL_RECORD_BYTES 8U
#define OTA_BOOT_JOURNAL_BYTES (OTA_BOOT_JOURNAL_RECORDS_OFFSET + 2U * OTA_BOOT_JOURNAL_RECORD_BYTES)
#define OTA_BOOT_JOURNAL_NAME "install-pending"

struct ota_boot_reader {
    void *context;
    /* Read ONLY partition3(kernel) or4(rootfs), relative offsets, including
     * their full erased-FF tails. A production
     * adapter must pin/validate geometry and device identity before use. */
    ssize_t (*read)(void *,unsigned mtd,uint32_t offset,void *,size_t);
};
struct ota_boot_result {
    uint64_t sequence;
    unsigned already_accepted,accepted_committed,cleanup_complete;
};
/* Optional trusted test seam. Nonzero injects a failure at the named durability
 * boundary. Production passes NULL. It cannot authorize an otherwise invalid
 * journal, signature, installed payload, health or boot identity. */
typedef int (*ota_boot_fault)(void *,const char *point);

/* Installer calls this BEFORE EVERY apply attempt, with the current Linux
 * boot_id parsed into16bytes. It fully re-verifies the retained capsule and
 * pending identity. Publishes a bounded durable journal; NEVER writes MTD or
 * advances accepted. A retry in a later boot refreshes its installer boot ID. */
int ota_boot_prepare(struct ota_apply_plan *,const uint8_t installer_boot_id[16],
                     ota_boot_fault,void *,char *error,size_t error_size);

/* Run BEFORE ota_state_open/control_update_init: accepted may already equal
 * pending after an interrupted prior confirmation. directory and lock_fd are
 * existing owned private state/lock FDs; this function acquires/retains an
 * exclusive flock on that exact named lock, never closes passed FDs.
 * Caller must also serialize its own threads sharing that lock description.
 * Caller must attest ACTUAL media health in this boot (not a browser boolean).
 * It must separately attest installed-firmware boot origin from the NOR-root
 * PID1 bootstrap and kernel boot arguments, rejecting SD/recovery/trial boots.
 * A changed boot ID alone never proves that the installed image was booted.
 * The current kernel boot ID must differ from the journal's installer ID.
 * Advancing accepted requires media_healthy==1. Cleanup of an already durable
 * accepted==journal sequence may recover with media_healthy==0; that path
 * never advances accepted and still re-verifies boot origin and installed bytes.
 * Reconstructs/hashes actual installed canonical package through read-only
 * callbacks; self-reported versions never prove installation.
 * Returns1 confirmed/recovered,0 no install journal (staging isn't applied),
 * -1 failure. On failure accepted MAY have atomically advanced; inspect result
 * and retry before opening the ordinary staging state. Stage storage must be
 * fresh RAM after boot; this module never touches a surviving pending.cap.
 * No syscall in this module opens, erases, or writes a physical MTD device. */
int ota_boot_confirm(int directory,int lock_fd,const uint8_t key[32],
                     const uint8_t current_boot_id[16],int booted_installed_firmware,
                     int media_healthy,
                     const struct ota_boot_reader *,ota_boot_fault,void *,
                     struct ota_boot_result *,char *error,size_t error_size);
#endif
