#ifndef ANYKA_UPDATE_CAPSULE_H
#define ANYKA_UPDATE_CAPSULE_H
#include <stddef.h>
#include <stdint.h>
#define ANYKA_UPDATE_BOARD CAMERA_BOARD_ID
#define ANYKA_UPDATE_HEADER 192u
#define ANYKA_APP_BYTES 0x500000u
#define ANYKA_APP_ERASE 0x1000u
struct anyka_update_verified { uint64_t sequence; uint8_t digest[64]; };
/* Full exact-size immutable RAM view authenticated before any mutation. */
int anyka_update_verify(const uint8_t *,size_t,const uint8_t public_key[32],
                        uint64_t accepted,struct anyka_update_verified *);
void anyka_update_digest(uint8_t out[64],const void *,size_t);
void anyka_update_hex(char out[129],const uint8_t digest[64]);
struct anyka_update_flash {
 void *context;
 int (*erase)(void *,uint32_t offset,uint32_t size);
 int (*write)(void *,uint32_t offset,const void *,size_t);
 int (*read)(void *,uint32_t offset,void *,size_t);
 int (*progress)(void *,unsigned completed,unsigned total);
};
/* Verifies signature and complete payload before first erase. Every block is
 * read back immediately. Caller must keep the source immutable throughout;
 * production uses a kernel read lease. Backend exposes only APP-relative offsets. */
int anyka_update_apply(const uint8_t *,size_t,const uint8_t public_key[32],
                       uint64_t accepted,const struct anyka_update_flash *);
#endif
