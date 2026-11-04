#ifndef CAMERA_OTA_STATE_H
#define CAMERA_OTA_STATE_H
#include "ota_core.h"

struct ota_state {
    int directory;
    int staging;
    int lock;
    uint8_t key[32];
    uint64_t accepted;
    int has_pending;
    struct ota_verified pending;
};

int ota_state_initialize(const char *directory, uint64_t accepted,
                         char *error, size_t error_size);
int ota_state_open(struct ota_state *state, const char *directory, const char *staging,
                   const uint8_t key[32], char *error, size_t error_size);
void ota_state_close(struct ota_state *state);
int ota_state_refresh(struct ota_state *state, char *error, size_t error_size);
/* 1 + verified open FD, 0 if RAM-stage missing, -1 if present but invalid. */
int ota_state_staged(struct ota_state *state, int *fd, char *error, size_t error_size);
int ota_state_upload(struct ota_state *state, char *name, size_t name_size,
                     char *error, size_t error_size);
int ota_state_publish(struct ota_state *state, int fd, const char *temporary,
                      const struct ota_verified *verified, int *resumed,
                      char *error, size_t error_size);
#endif
