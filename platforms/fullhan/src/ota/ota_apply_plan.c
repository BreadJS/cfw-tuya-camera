/* Authenticated, FD-bound apply planning and callback-only execution mechanics.
 * No physical-device open/write/ioctl/reboot implementation exists here.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "ota_apply_plan.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static const char *const names[OTA_MTD_COUNT] = {
    "bootstrap", "uboot-env", "uboot", "kernel", "rootfs", "config"
};
static const uint32_t sizes[OTA_MTD_COUNT] = {
    0x40000, 0x10000, 0x40000, OTA_KERNEL_LIMIT, OTA_ROOTFS_LIMIT, 0x50000
};
static int fail(char *error, size_t size, const char *message)
{
    if (size) snprintf(error, size, "%s", message);
    return -1;
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 |
        (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static int pread_exact(int fd, uint8_t *out, size_t size, uint64_t offset)
{
    size_t done = 0;
    while (done < size) {
        ssize_t got = pread(fd, out + done, size - done, (off_t)(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}
static int private_file(int fd, uint64_t maximum)
{
    struct stat st;
    return fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1 || st.st_size < 0 || (uint64_t)st.st_size > maximum;
}
static int existing_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd); return -1;
    }
    return fd;
}
static int read_state_file(int directory, const char *name, uint8_t *data, size_t maximum, size_t *length)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (private_file(fd, maximum) || fstat(fd, &st) ||
        pread_exact(fd, data, (size_t)st.st_size, 0)) { close(fd); return -1; }
    *length = (size_t)st.st_size;
    struct stat after;
    int changed = fstat(fd, &after) || after.st_size != st.st_size ||
        after.st_mtim.tv_sec != st.st_mtim.tv_sec || after.st_mtim.tv_nsec != st.st_mtim.tv_nsec;
    close(fd);
    return changed ? -1 : 0;
}
static int load_journal(struct ota_apply_plan *plan, uint64_t *accepted, uint8_t pending[OTA_HEADER_SIZE])
{
    uint8_t text[22];
    size_t length;
    if (read_state_file(plan->state_dir, "accepted", text, 21, &length) || length < 2 ||
        text[length - 1] != '\n') return -1;
    for (size_t i = 0; i + 1 < length; ++i)
        if (text[i] < '0' || text[i] > '9') return -1;
    text[length - 1] = 0;
    if (ota_parse_u64((const char *)text, accepted)) return -1;
    if (read_state_file(plan->state_dir, "pending", pending, OTA_HEADER_SIZE, &length) ||
        length != OTA_HEADER_SIZE) return -1;
    return 0;
}

int ota_mtd_validate(const struct ota_mtd_map *map, char *error, size_t size)
{
    uint32_t offset = 0;
    for (unsigned i = 0; i < OTA_MTD_COUNT; ++i) {
        const struct ota_mtd_partition *part = &map->part[i];
        if (part->index != i || part->size != sizes[i] || part->erase_size != OTA_ERASE_SIZE ||
            part->flash_offset != offset || memchr(part->name, 0, sizeof part->name) == NULL ||
            strcmp(part->name, names[i])) return fail(error, size, "MTD map differs from exact camera layout");
        offset += part->size;
    }
    return offset == OTA_FLASH_SIZE ? 0 : fail(error, size, "MTD total is not 8 MiB");
}
int ota_mtd_equal(const struct ota_mtd_map *a, const struct ota_mtd_map *b)
{
    for (unsigned i = 0; i < OTA_MTD_COUNT; ++i) {
        if (a->part[i].index != b->part[i].index || a->part[i].size != b->part[i].size ||
            a->part[i].erase_size != b->part[i].erase_size ||
            a->part[i].flash_offset != b->part[i].flash_offset ||
            memcmp(a->part[i].name, b->part[i].name, sizeof a->part[i].name)) return 0;
    }
    return 1;
}

static char *spaces(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static int hex8(char **text, uint32_t *value)
{
    uint32_t result = 0;
    char *p = *text;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned c = (unsigned char)*p++;
        unsigned digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit > 15) return -1;
        result = (result << 4) | digit;
    }
    if (*p != ' ' && *p != '\t') return -1;
    *value = result; *text = spaces(p); return 0;
}
int ota_mtd_read(const char *path, struct ota_mtd_map *map, char *error, size_t error_size)
{
    uint8_t raw[4097];
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return fail(error, error_size, "cannot open MTD map");
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return fail(error, error_size, "MTD map is not regular/proc input"); }
    size_t used = 0;
    while (used < sizeof raw) {
        ssize_t got = read(fd, raw + used, sizeof raw - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { close(fd); return fail(error, error_size, "MTD map read failed"); }
        if (!got) break;
        used += (size_t)got;
    }
    close(fd);
    if (!used || used > 4096 || raw[used - 1] != '\n' || memchr(raw, 0, used))
        return fail(error, error_size, "MTD map length/encoding is invalid");
    memset(map, 0, sizeof *map);
    crypto_blake2b(map->raw_digest, 64, raw, used);
    raw[used] = 0;
    char *save, *line = strtok_r((char *)raw, "\n", &save);
    if (!line || strcmp(line, "dev:    size   erasesize  name"))
        return fail(error, error_size, "unexpected /proc/mtd header");
    unsigned count = 0;
    uint32_t offset = 0;
    while ((line = strtok_r(NULL, "\n", &save))) {
        if (count >= OTA_MTD_COUNT || strlen(line) > 128 || strncmp(line, "mtd", 3) ||
            line[3] != (char)('0' + count) || line[4] != ':')
            return fail(error, error_size, "extra/duplicate/reordered/invalid MTD index");
        char *p = spaces(line + 5);
        struct ota_mtd_partition *part = &map->part[count];
        part->index = count;
        part->flash_offset = offset;
        if (hex8(&p, &part->size) || hex8(&p, &part->erase_size) || *p++ != '"')
            return fail(error, error_size, "malformed MTD geometry");
        char *end = strchr(p, '"');
        if (!end || end == p || (size_t)(end - p) >= sizeof part->name || *spaces(end + 1))
            return fail(error, error_size, "malformed MTD name");
        memcpy(part->name, p, (size_t)(end - p));
        offset += part->size;
        count++;
    }
    if (count != OTA_MTD_COUNT) return fail(error, error_size, "missing MTD partitions");
    return ota_mtd_validate(map, error, error_size);
}

static int source_identity(struct ota_apply_plan *plan)
{
    struct stat current, named;
    const struct stat *original = &plan->source_identity;
    if (private_file(plan->capsule_fd, OTA_CAPSULE_LIMIT) || fstat(plan->capsule_fd, &current) ||
        fstatat(plan->stage_dir, "pending.cap", &named, AT_SYMLINK_NOFOLLOW)) return -1;
    return current.st_dev != original->st_dev || current.st_ino != original->st_ino ||
        named.st_dev != current.st_dev || named.st_ino != current.st_ino || !S_ISREG(named.st_mode) ||
        current.st_size != original->st_size || current.st_mtim.tv_sec != original->st_mtim.tv_sec ||
        current.st_mtim.tv_nsec != original->st_mtim.tv_nsec || current.st_ctim.tv_sec != original->st_ctim.tv_sec ||
        current.st_ctim.tv_nsec != original->st_ctim.tv_nsec ? -1 : 0;
}
int ota_apply_plan_guard(struct ota_apply_plan *plan, char *error, size_t size)
{
    uint8_t pending[OTA_HEADER_SIZE];
    uint64_t accepted;
    if (load_journal(plan, &accepted, pending) || accepted != plan->accepted ||
        memcmp(pending, plan->verified.header, OTA_HEADER_SIZE))
        return fail(error, size, "trusted accepted/pending state changed");
    return source_identity(plan) ? fail(error, size, "retained capsule inode/metadata/path changed") : 0;
}

static int build_blocks(struct ota_apply_plan *plan, char *error, size_t error_size)
{
    uint8_t vendor[32], record[12], buffer[4096], digest[64];
    uint64_t position = OTA_HEADER_SIZE, end = OTA_HEADER_SIZE + plan->verified.payload_size;
    crypto_blake2b_ctx payload_hash;
    crypto_blake2b_init(&payload_hash, 64);
    if (pread_exact(plan->capsule_fd, vendor, sizeof vendor, position))
        return fail(error, error_size, "cannot read verified vendor header");
    crypto_blake2b_update(&payload_hash, vendor, sizeof vendor);
    position += sizeof vendor;
    uint32_t previous = 0;
    while (position < end) {
        if (end - position < sizeof record || pread_exact(plan->capsule_fd, record, sizeof record, position))
            return fail(error, error_size, "cannot read verified image record");
        crypto_blake2b_update(&payload_hash, record, sizeof record);
        position += sizeof record;
        uint32_t kind = be32(record), bytes = be32(record + 4);
        if ((kind != 2 && kind != 3) || kind <= previous || be32(record + 8) || bytes > end - position)
            return fail(error, error_size, "verified record changed while planning");
        uint32_t mtd = kind + 1;
        const struct ota_mtd_partition *part = &plan->map.part[mtd];
        if (!bytes || bytes > part->size) return fail(error, error_size, "image no longer fits selected partition");
        uint64_t image_start = position;
        for (uint32_t offset = 0; offset < part->size; offset += part->erase_size) {
            if (plan->block_count >= OTA_PLAN_BLOCKS) return fail(error, error_size, "too many planned blocks");
            struct ota_apply_block *block = &plan->blocks[plan->block_count++];
            memset(block, 0, sizeof *block);
            block->image_type = kind; block->mtd_index = mtd; block->partition_offset = offset;
            block->erase_bytes = part->erase_size;
            block->data_bytes = offset < bytes ? bytes - offset : 0;
            if (block->data_bytes > part->erase_size) block->data_bytes = part->erase_size;
            if (block->data_bytes) block->capsule_offset = image_start + offset;
            crypto_blake2b_ctx block_hash;
            crypto_blake2b_init(&block_hash, 64);
            uint32_t done = 0;
            while (done < block->data_bytes) {
                size_t chunk = block->data_bytes - done;
                if (chunk > sizeof buffer) chunk = sizeof buffer;
                if (pread_exact(plan->capsule_fd, buffer, chunk, image_start + offset + done))
                    return fail(error, error_size, "source became unreadable while planning");
                crypto_blake2b_update(&payload_hash, buffer, chunk);
                crypto_blake2b_update(&block_hash, buffer, chunk);
                done += (uint32_t)chunk;
            }
            memset(buffer, 0xff, sizeof buffer);
            while (done < block->erase_bytes) {
                size_t chunk = block->erase_bytes - done;
                if (chunk > sizeof buffer) chunk = sizeof buffer;
                crypto_blake2b_update(&block_hash, buffer, chunk);
                done += (uint32_t)chunk;
            }
            crypto_blake2b_final(&block_hash, block->expected_digest);
        }
        position += bytes;
        previous = kind;
    }
    crypto_blake2b_final(&payload_hash, digest);
    if (position != end || crypto_verify64(digest, plan->verified.header + 64))
        return fail(error, error_size, "planned bytes differ from signed payload digest");
    return ota_apply_plan_guard(plan, error, error_size);
}

int ota_apply_plan_open(struct ota_apply_plan *plan, const char *key_path,
                         const char *state_path, const char *stage_path,
                         const char *mtd_path, char *error, size_t size)
{
    memset(plan, 0, sizeof *plan);
    plan->state_dir = plan->stage_dir = plan->lock_fd = plan->capsule_fd = -1;
    if (ota_read_public_key(key_path, plan->key, 1, error, size)) goto failed;
    plan->state_dir = existing_directory(state_path);
    plan->stage_dir = existing_directory(stage_path);
    if (plan->state_dir < 0 || plan->stage_dir < 0) { fail(error, size, "existing private state/stage directories required"); goto failed; }
    struct stat a, b;
    if (fstat(plan->state_dir, &a) || fstat(plan->stage_dir, &b) ||
        (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) { fail(error, size, "state and stage directories must differ"); goto failed; }
    plan->lock_fd = openat(plan->state_dir, "lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (plan->lock_fd < 0 || private_file(plan->lock_fd, 0) || flock(plan->lock_fd, LOCK_EX | LOCK_NB)) {
        fail(error, size, "trusted existing state lock unavailable"); goto failed;
    }
    uint8_t pending[OTA_HEADER_SIZE];
    if (load_journal(plan, &plan->accepted, pending)) { fail(error, size, "trusted accepted/pending journal unavailable"); goto failed; }
    plan->capsule_fd = openat(plan->stage_dir, "pending.cap", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (plan->capsule_fd < 0 || private_file(plan->capsule_fd, OTA_CAPSULE_LIMIT) ||
        flock(plan->capsule_fd, LOCK_SH | LOCK_NB) || fstat(plan->capsule_fd, &plan->source_identity)) {
        fail(error, size, "private retained pending capsule unavailable"); goto failed;
    }
    if (ota_verify_fd(plan->capsule_fd, plan->key, plan->accepted, &plan->verified, error, size)) goto failed;
    if (memcmp(pending, plan->verified.header, OTA_HEADER_SIZE)) { fail(error, size, "capsule does not match authenticated pending journal"); goto failed; }
    if (ota_mtd_read(mtd_path, &plan->map, error, size) || build_blocks(plan, error, size)) goto failed;
    return 0;
failed:
    ota_apply_plan_close(plan); return -1;
}
int ota_apply_plan_open_locked(struct ota_apply_plan *plan,int state,int stage,
                         int lock,const uint8_t key[32],const char *mtd_path,
                         char *error,size_t size)
{
    memset(plan,0,sizeof *plan);
    plan->state_dir=plan->stage_dir=plan->lock_fd=plan->capsule_fd=-1;
    if(!key||state<0||stage<0||lock<0)return fail(error,size,"trusted state descriptors required");
    struct stat a,b,l,named;
    if(fstat(state,&a)||fstat(stage,&b)||!S_ISDIR(a.st_mode)||!S_ISDIR(b.st_mode)||
       a.st_uid!=geteuid()||b.st_uid!=geteuid()||(a.st_mode&077)||(b.st_mode&077)||
       (a.st_dev==b.st_dev&&a.st_ino==b.st_ino)||private_file(lock,0)||
       fstat(lock,&l)||fstatat(state,"lock",&named,AT_SYMLINK_NOFOLLOW)||
       !S_ISREG(named.st_mode)||l.st_dev!=named.st_dev||l.st_ino!=named.st_ino)
        return fail(error,size,"private descriptor/named lock identity mismatch");
    plan->state_dir=fcntl(state,F_DUPFD_CLOEXEC,3);
    plan->stage_dir=fcntl(stage,F_DUPFD_CLOEXEC,3);
    plan->lock_fd=fcntl(lock,F_DUPFD_CLOEXEC,3);
    if(plan->state_dir<0||plan->stage_dir<0||plan->lock_fd<0||
       flock(plan->lock_fd,LOCK_EX|LOCK_NB)){
        fail(error,size,"exclusive state handoff unavailable");goto failed;
    }
    memcpy(plan->key,key,32);
    uint8_t pending[OTA_HEADER_SIZE];
    if(load_journal(plan,&plan->accepted,pending)){
        fail(error,size,"trusted accepted/pending journal unavailable");goto failed;
    }
    plan->capsule_fd=openat(plan->stage_dir,"pending.cap",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(plan->capsule_fd<0||private_file(plan->capsule_fd,OTA_CAPSULE_LIMIT)||
       flock(plan->capsule_fd,LOCK_SH|LOCK_NB)||fstat(plan->capsule_fd,&plan->source_identity)){
        fail(error,size,"private retained capsule unavailable");goto failed;
    }
    if(ota_verify_fd(plan->capsule_fd,plan->key,plan->accepted,&plan->verified,error,size))goto failed;
    if(memcmp(pending,plan->verified.header,OTA_HEADER_SIZE)){
        fail(error,size,"capsule does not match signed pending journal");goto failed;
    }
    if(ota_mtd_read(mtd_path,&plan->map,error,size)||build_blocks(plan,error,size))goto failed;
    return 0;
failed:
    ota_apply_plan_close(plan);return -1;
}
void ota_apply_plan_close(struct ota_apply_plan *plan)
{
    if (plan->capsule_fd >= 0) close(plan->capsule_fd);
    if (plan->lock_fd >= 0) close(plan->lock_fd);
    if (plan->stage_dir >= 0) close(plan->stage_dir);
    if (plan->state_dir >= 0) close(plan->state_dir);
    plan->state_dir = plan->stage_dir = plan->lock_fd = plan->capsule_fd = -1;
}

static int backend_map(struct ota_apply_plan *plan, const struct ota_apply_backend *backend,
                        char *error, size_t size)
{
    struct ota_mtd_map actual;
    memset(&actual, 0, sizeof actual);
    if (backend->geometry(backend->context, &actual) || ota_mtd_validate(&actual, error, size) ||
        !ota_mtd_equal(&actual, &plan->map))
        return fail(error, size, "backend geometry differs from authenticated plan");
    return 0;
}
int ota_apply_plan_run(struct ota_apply_plan *plan, const struct ota_apply_backend *backend,
                       struct ota_apply_progress *progress, char *error, size_t size)
{
    uint8_t block_bytes[OTA_ERASE_SIZE], readback[4096], digest[64];
    struct ota_verified fresh;
    if (!plan || !progress) return fail(error, size, "valid plan/progress objects required");
    memset(progress, 0, sizeof *progress);
    if (!plan->block_count || plan->block_count > OTA_PLAN_BLOCKS || plan->capsule_fd < 0)
        return fail(error, size, "invalid bounded block plan");
    if (!backend || !backend->geometry || !backend->erase || !backend->write || !backend->read)
        return fail(error, size, "complete explicitly supplied backend required");
    if (ota_apply_plan_guard(plan, error, size) ||
        ota_verify_fd(plan->capsule_fd, plan->key, plan->accepted, &fresh, error, size) ||
        memcmp(fresh.header, plan->verified.header, OTA_HEADER_SIZE) || backend_map(plan, backend, error, size))
        return fail(error, size, "pre-execution authentication/state/geometry check failed");
    /* Reconstruct from the same authenticated FD before any backend mutation.
     * This catches corrupted/edited plan entries even late in the operation
     * list, instead of discovering them only after earlier blocks changed. */
    struct ota_apply_plan canonical = *plan;
    canonical.block_count = 0;
    if (build_blocks(&canonical, error, size) || canonical.block_count != plan->block_count ||
        memcmp(canonical.blocks, plan->blocks, plan->block_count * sizeof plan->blocks[0]))
        return fail(error, size, "block plan no longer matches authenticated capsule");
    for (unsigned index = 0; index < plan->block_count; ++index) {
        const struct ota_apply_block *block = &plan->blocks[index];
        if (ota_apply_plan_guard(plan, error, size) || backend_map(plan, backend, error, size)) return -1;
        if ((block->mtd_index != 3 && block->mtd_index != 4) || block->erase_bytes != OTA_ERASE_SIZE ||
            block->partition_offset % OTA_ERASE_SIZE || block->data_bytes > block->erase_bytes ||
            block->partition_offset > plan->map.part[block->mtd_index].size - block->erase_bytes)
            return fail(error, size, "unsafe planned partition/range");
        memset(block_bytes, 0xff, sizeof block_bytes);
        if (block->data_bytes && pread_exact(plan->capsule_fd, block_bytes, block->data_bytes, block->capsule_offset))
            return fail(error, size, "cannot snapshot planned image block");
        crypto_blake2b(digest, sizeof digest, block_bytes, sizeof block_bytes);
        if (crypto_verify64(digest, block->expected_digest))
            return fail(error, size, "block snapshot differs from authenticated plan");
        if (backend->erase(backend->context, block->mtd_index, block->partition_offset, block->erase_bytes))
            return fail(error, size, "backend erase failed; target may be partially changed");
        progress->bytes_erased += block->erase_bytes;
        for (uint32_t offset = 0; offset < block->data_bytes;) {
            size_t bytes = block->data_bytes - offset;
            if (bytes > sizeof readback) bytes = sizeof readback;
            if (backend->write(backend->context, block->mtd_index, block->partition_offset + offset,
                               block_bytes + offset, bytes))
                return fail(error, size, "backend write failed; target may be partially changed");
            progress->bytes_written += bytes;
            offset += (uint32_t)bytes;
        }
        for (uint32_t offset = 0; offset < block->erase_bytes; offset += sizeof readback) {
            if (backend->read(backend->context, block->mtd_index, block->partition_offset + offset,
                              readback, sizeof readback))
                return fail(error, size, "backend readback failed; target may be partially changed");
            progress->bytes_read_back += sizeof readback;
            if (memcmp(readback, block_bytes + offset, sizeof readback))
                return fail(error, size, "readback bytes mismatch; target may be partially changed");
        }
        progress->blocks_completed++;
    }
    return ota_apply_plan_guard(plan, error, size);
}
