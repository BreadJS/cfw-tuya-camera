#ifndef CAMERA_OTA_CORE_H
#define CAMERA_OTA_CORE_H
#include <stddef.h>
#include <stdint.h>

#define OTA_HEADER_SIZE 192U
#define OTA_KERNEL_LIMIT UINT32_C(0x1d0000)
#define OTA_ROOTFS_LIMIT UINT32_C(0x550000)
#define OTA_PAYLOAD_LIMIT (32U + 24U + OTA_KERNEL_LIMIT + OTA_ROOTFS_LIMIT)
#define OTA_CAPSULE_LIMIT (OTA_HEADER_SIZE + OTA_PAYLOAD_LIMIT)
#define OTA_BOARD_ID CAMERA_BOARD_ID

struct ota_verified {
    uint8_t header[OTA_HEADER_SIZE];
    uint64_t sequence;
    uint64_t payload_size;
    unsigned records;
};

int ota_parse_u64(const char *text, uint64_t *value);
int ota_read_public_key(const char *path, uint8_t key[32], int protected_file,
                        char *error, size_t error_size);
int ota_verify_header(const uint8_t header[OTA_HEADER_SIZE], const uint8_t key[32],
                      uint64_t current, struct ota_verified *verified,
                      char *error, size_t error_size);
/* Preads one retained regular-file FD with bounded buffers, never reopens it. */
int ota_verify_fd(int fd, const uint8_t key[32], uint64_t current,
                  struct ota_verified *verified, char *error, size_t error_size);
/* New output only; hashes the copied bytes again before reporting success. */
int ota_extract_fd(int fd, const struct ota_verified *verified, const char *path,
                   char *error, size_t error_size);
#endif
