#ifndef CAMERA_OTA_APPLY_PLAN_H
#define CAMERA_OTA_APPLY_PLAN_H
#include "ota_core.h"
#include <sys/stat.h>

#define OTA_MTD_COUNT 6U
#define OTA_ERASE_SIZE UINT32_C(0x10000)
#define OTA_FLASH_SIZE UINT32_C(0x800000)
#define OTA_PLAN_BLOCKS (29U + 85U)

struct ota_mtd_partition {
    uint32_t index;
    uint32_t size;
    uint32_t erase_size;
    uint32_t flash_offset;
    char name[32];
};
struct ota_mtd_map {
    struct ota_mtd_partition part[OTA_MTD_COUNT];
    uint8_t raw_digest[64];
};
struct ota_apply_block {
    uint32_t image_type;
    uint32_t mtd_index;
    uint32_t partition_offset;
    uint32_t erase_bytes;
    uint32_t data_bytes;
    uint64_t capsule_offset; /* zero when data_bytes is zero: erased FF tail */
    uint8_t expected_digest[64]; /* whole erase block: image data then FF */
};
struct ota_apply_plan {
    int state_dir;
    int stage_dir;
    int lock_fd;
    int capsule_fd;
    uint8_t key[32];
    uint64_t accepted;
    struct ota_verified verified;
    struct stat source_identity;
    struct ota_mtd_map map;
    unsigned block_count;
    struct ota_apply_block blocks[OTA_PLAN_BLOCKS];
};

int ota_mtd_read(const char *path, struct ota_mtd_map *map, char *error, size_t error_size);
int ota_mtd_validate(const struct ota_mtd_map *map, char *error, size_t error_size);
int ota_mtd_equal(const struct ota_mtd_map *a, const struct ota_mtd_map *b);

/* Opens existing private directories and the existing state lock read-only.
 * No directory creation, journal recovery, cleanup, or state changes occur.
 * The lock, capsule FD and exact authenticated pending identity remain held.
 */
int ota_apply_plan_open(struct ota_apply_plan *plan, const char *key_path,
                         const char *state_dir, const char *stage_dir,
                         const char *mtd_map_path, char *error, size_t error_size);
/* Trusted service/exec handoff only: duplicate the already-owned directory
 * descriptors and the SAME open description holding the exclusive state
 * flock. Revalidates ownership, named-lock identity, full capsule signature,
 * journal, source identity and block hashes. No physical device access.
 * Caller must exclude concurrent operations in its own service. */
int ota_apply_plan_open_locked(struct ota_apply_plan *,int state_directory,
                         int stage_directory,int lock_fd,const uint8_t key[32],
                         const char *mtd_map_path,char *error,size_t error_size);
void ota_apply_plan_close(struct ota_apply_plan *plan);
int ota_apply_plan_guard(struct ota_apply_plan *plan, char *error, size_t error_size);

/* Callbacks are trusted implementation
 * code: they must operate only the passed partition-relative ranges and return
 * zero only after completing the full operation. Failure may be partial.
 * geometry must independently query the actual device layout before execution.
 */
struct ota_apply_backend {
    void *context;
    int (*geometry)(void *context, struct ota_mtd_map *map);
    int (*erase)(void *context, uint32_t mtd, uint32_t offset, uint32_t size);
    int (*write)(void *context, uint32_t mtd, uint32_t offset, const uint8_t *data, size_t size);
    int (*read)(void *context, uint32_t mtd, uint32_t offset, uint8_t *data, size_t size);
};
struct ota_apply_progress {
    unsigned blocks_completed;
    uint64_t bytes_erased;
    uint64_t bytes_written;
    uint64_t bytes_read_back;
};
int ota_apply_plan_run(struct ota_apply_plan *plan, const struct ota_apply_backend *backend,
                       struct ota_apply_progress *progress, char *error, size_t error_size);
#endif
