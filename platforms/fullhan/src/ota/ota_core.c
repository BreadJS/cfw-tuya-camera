/* Streaming authentication/parser core. No network, flash or update triggers. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "ota_core.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t magic[8] = {'F','H','C','A','M','O','1',0};
static const uint8_t board[32] = OTA_BOARD_ID;
static const uint8_t platform[20] = "Fuhan8626V100";

static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (size) vsnprintf(error, size, format, args);
    va_end(args);
    return -1;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 |
        (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static uint32_t crc_update(uint32_t value, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        value ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (UINT32_C(0xedb88320) & (0U - (value & 1U)));
    }
    return value;
}
static int read_exact_at(int fd, uint8_t *out, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size) {
        ssize_t got = pread(fd, out + done, size - done, offset + (off_t)done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}

int ota_parse_u64(const char *text, uint64_t *value)
{
    uint64_t result = 0;
    if (!text || !*text || !value) return -1;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (unsigned)(*p - '0')) / 10)
            return -1;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *value = result;
    return 0;
}

int ota_read_public_key(const char *path, uint8_t key[32], int protected_file,
                        char *error, size_t error_size)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return fail(error, error_size, "cannot open public key");
    int rc = fstat(fd, &st);
    if (rc || !S_ISREG(st.st_mode) || st.st_size != 32 ||
        (protected_file && (st.st_uid != geteuid() || (st.st_mode & 022)))) {
        close(fd);
        return fail(error, error_size, "key must be a trusted 32-byte regular file");
    }
    rc = read_exact_at(fd, key, 32, 0);
    close(fd);
    return rc ? fail(error, error_size, "cannot read public key") : 0;
}

int ota_verify_header(const uint8_t header[OTA_HEADER_SIZE], const uint8_t key[32],
                      uint64_t current, struct ota_verified *verified,
                      char *error, size_t error_size)
{
    if (memcmp(header, magic, 8) || le32(header + 8) != 1 ||
        le32(header + 12) != OTA_HEADER_SIZE || memcmp(header + 16, board, 32))
        return fail(error, error_size, "capsule format or hardware identity mismatch");
    uint64_t sequence = le64(header + 48), length = le64(header + 56);
    if (!sequence || sequence <= current)
        return fail(error, error_size, "replayed/rolled-back sequence");
    if (length < 32 || length > OTA_PAYLOAD_LIMIT)
        return fail(error, error_size, "capsule payload length out of bounds");
    if (crypto_ed25519_check(header + 128, key, header, 128))
        return fail(error, error_size, "Ed25519 signature mismatch");
    memset(verified, 0, sizeof *verified);
    memcpy(verified->header, header, OTA_HEADER_SIZE);
    verified->sequence = sequence;
    verified->payload_size = length;
    return 0;
}

struct reader {
    int fd;
    off_t position;
    uint64_t remaining;
    crypto_blake2b_ctx digest;
};
static int take(struct reader *reader, uint8_t *out, size_t size)
{
    if (size > reader->remaining || read_exact_at(reader->fd, out, size, reader->position))
        return -1;
    crypto_blake2b_update(&reader->digest, out, size);
    reader->position += (off_t)size;
    reader->remaining -= size;
    return 0;
}

static int image(struct reader *reader, uint32_t kind, uint32_t size,
                  char *error, size_t error_size)
{
    uint8_t first[96], buffer[4096];
    uint32_t crc = UINT32_MAX, expected_crc = 0;
    size_t first_size;
    if (kind == 2) {
        first_size = 64;
        if (size < first_size || size > OTA_KERNEL_LIMIT)
            return fail(error, error_size, "invalid kernel size");
    } else if (kind == 3) {
        first_size = 96;
        if (size < first_size || size > OTA_ROOTFS_LIMIT)
            return fail(error, error_size, "invalid rootfs size");
    } else return fail(error, error_size, "only kernel/rootfs records are allowed");
    if (take(reader, first, first_size)) return fail(error, error_size, "truncated image header");
    if (kind == 2) {
        uint32_t header_crc = be32(first + 4);
        memset(first + 4, 0, 4);
        if (be32(first) != UINT32_C(0x27051956) ||
            ~crc_update(UINT32_MAX, first, 64) != header_crc)
            return fail(error, error_size, "kernel header CRC/magic");
        if (be32(first + 12) != size - 64 || first[28] != 5 || first[29] != 2 ||
            first[30] != 2 || first[31] || be32(first + 16) != UINT32_C(0xa0008000) ||
            be32(first + 20) != UINT32_C(0xa0008000))
            return fail(error, error_size, "kernel architecture/load/entry/length mismatch");
        expected_crc = be32(first + 24);
    } else if (memcmp(first, "hsqs", 4) || first[28] != 4 || first[29] ||
               first[30] || first[31] || first[20] != 4 || first[21] ||
               le64(first + 40) != size) {
        return fail(error, error_size, "rootfs type/version/compression/size mismatch");
    }
    uint32_t remaining = size - (uint32_t)first_size;
    while (remaining) {
        size_t bytes = remaining > sizeof buffer ? sizeof buffer : remaining;
        if (take(reader, buffer, bytes)) return fail(error, error_size, "truncated image body");
        if (kind == 2) crc = crc_update(crc, buffer, bytes);
        remaining -= (uint32_t)bytes;
    }
    if (kind == 2 && ~crc != expected_crc)
        return fail(error, error_size, "kernel payload CRC");
    return 0;
}

int ota_verify_fd(int fd, const uint8_t key[32], uint64_t current,
                  struct ota_verified *verified, char *error, size_t error_size)
{
    uint8_t header[OTA_HEADER_SIZE], vendor[32], record[12], digest[64];
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < OTA_HEADER_SIZE ||
        (uint64_t)before.st_size > OTA_CAPSULE_LIMIT || read_exact_at(fd, header, sizeof header, 0))
        return fail(error, error_size, "input is not a bounded regular capsule");
    if (ota_verify_header(header, key, current, verified, error, error_size)) return -1;
    if (verified->payload_size != (uint64_t)before.st_size - OTA_HEADER_SIZE)
        return fail(error, error_size, "capsule payload length mismatch");
    struct reader reader = {.fd = fd, .position = OTA_HEADER_SIZE,
                             .remaining = verified->payload_size};
    crypto_blake2b_init(&reader.digest, sizeof digest);
    if (take(&reader, vendor, sizeof vendor) || memcmp(vendor, platform, 20) ||
        be32(vendor + 20) != UINT32_C(0xabcd1234))
        return fail(error, error_size, "noncanonical vendor package header");
    for (unsigned i = 24; i < 32; ++i)
        if (vendor[i]) return fail(error, error_size, "noncanonical reserved vendor bytes");
    uint32_t previous = 0;
    while (reader.remaining) {
        if (take(&reader, record, sizeof record)) return fail(error, error_size, "truncated record");
        uint32_t kind = be32(record), size = be32(record + 4);
        if (kind <= previous || be32(record + 8) || size > reader.remaining)
            return fail(error, error_size, "duplicate/unordered/truncated/nonzero-address record");
        if (image(&reader, kind, size, error, error_size)) return -1;
        previous = kind;
        verified->records++;
    }
    if (!verified->records) return fail(error, error_size, "empty vendor package");
    crypto_blake2b_final(&reader.digest, digest);
    if (crypto_verify64(digest, header + 64)) return fail(error, error_size, "payload digest mismatch");
    if (fstat(fd, &after) || after.st_size != before.st_size ||
        after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != before.st_ctim.tv_sec || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
        return fail(error, error_size, "capsule changed during verification");
    return 0;
}

int ota_extract_fd(int fd, const struct ota_verified *verified, const char *path,
                   char *error, size_t error_size)
{
    uint8_t header[OTA_HEADER_SIZE], buffer[4096], digest[64];
    if (read_exact_at(fd, header, sizeof header, 0) || memcmp(header, verified->header, sizeof header))
        return fail(error, error_size, "capsule header changed before extraction");
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) return fail(error, error_size, "output must be a new regular file");
    struct reader reader = {.fd = fd, .position = OTA_HEADER_SIZE,
                             .remaining = verified->payload_size};
    crypto_blake2b_init(&reader.digest, sizeof digest);
    int failed = 0;
    while (reader.remaining && !failed) {
        size_t bytes = reader.remaining > sizeof buffer ? sizeof buffer : (size_t)reader.remaining;
        if (take(&reader, buffer, bytes)) { failed = 1; break; }
        size_t done = 0;
        while (done < bytes) {
            ssize_t written = write(output, buffer + done, bytes - done);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { failed = 1; break; }
            done += (size_t)written;
        }
    }
    crypto_blake2b_final(&reader.digest, digest);
    if (crypto_verify64(digest, verified->header + 64) || fsync(output)) failed = 1;
    if (close(output)) failed = 1;
    if (failed) {
        unlink(path);
        return fail(error, error_size, "extraction write or digest verification failed");
    }
    return 0;
}
