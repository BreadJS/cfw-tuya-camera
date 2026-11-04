#ifndef ANYKA_UPDATE_RECEIPT_H
#define ANYKA_UPDATE_RECEIPT_H
#include <stddef.h>
#include <stdint.h>
struct anyka_update_receipt {
 uint64_t sequence;
 char board[40],digest[129],boot_id[40],install_boot_id[40],status[16];
 unsigned schema,media,web,network,app_root_boot;
};
/* Bounded flat JSON schema; rejects duplicate/unknown keys and escapes. */
int anyka_update_receipt_parse(const char *,size_t,int health,struct anyka_update_receipt *);
int anyka_update_can_confirm(const struct anyka_update_receipt *pending,
 const struct anyka_update_receipt *health,const char *current_boot_id,
 const char *actual_app_digest,uint64_t accepted);
#endif
