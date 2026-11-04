/* Trusted sequence/pending journal and private staging files. No firmware I/O. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "ota_state.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long counter;
static int fail(char *error, size_t size, const char *message)
{
    if (size) snprintf(error, size, "%s", message);
    return -1;
}
static int private_directory(const char *path, char *error, size_t size)
{
    if (mkdir(path, 0700) && errno != EEXIST) return fail(error, size, "cannot create private directory");
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return fail(error, size, "cannot open private directory");
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd);
        return fail(error, size, "directory must be owned by service uid, mode 0700");
    }
    return fd;
}
static int private_file(int fd)
{
    struct stat st;
    return fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1;
}
static int lock_directory(int directory, char *error, size_t size)
{
    int fd = openat(directory, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return fail(error, size, "cannot open state lock");
    if (private_file(fd) || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return fail(error, size, "state lock is unsafe or another service is active");
    }
    return fd;
}
static int read_private(int directory, const char *name, uint8_t *out, size_t limit, size_t *length)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (private_file(fd) || fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size > limit) {
        close(fd); return -1;
    }
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, out + done, (size_t)st.st_size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { close(fd); return -1; }
        done += (size_t)got;
    }
    uint8_t extra;
    ssize_t got;
    do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
    close(fd);
    if (got) return -1;
    *length = done;
    return 0;
}
static int new_temporary(int directory, const char *prefix, char *name, size_t size)
{
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int length = snprintf(name, size, "%s-%ld-%lu.tmp", prefix, (long)getpid(), ++counter);
        if (length < 0 || (size_t)length >= size) return -1;
        int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}
static int write_exact(int fd, const uint8_t *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t written = write(fd, data + done, size - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        done += (size_t)written;
    }
    return 0;
}
static int atomic_record(int directory, const char *name, const uint8_t *data, size_t size,
                          char *error, size_t error_size)
{
    struct stat st;
    if (!fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return fail(error, error_size, "journal target already exists or is inaccessible");
    char temporary[96];
    int fd = new_temporary(directory, "journal", temporary, sizeof temporary);
    if (fd < 0) return fail(error, error_size, "cannot create journal temporary");
    int bad = write_exact(fd, data, size) || fsync(fd);
    if (close(fd)) bad = 1;
    if (bad || renameat(directory, temporary, directory, name) || fsync(directory)) {
        unlinkat(directory, temporary, 0);
        return fail(error, error_size, "journal write/rename/sync failed");
    }
    return 0;
}
static int is_temporary(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || name[n] != '-') return 0;
    const char *p = name + n + 1;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    if (*p++ != '-' || *p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return !strcmp(p, ".tmp");
}
static int cleanup_temporary(int directory, const char *prefix)
{
    int copy = dup(directory);
    if (copy < 0) return -1;
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); return -1; }
    struct dirent *entry;
    int bad = 0;
    while ((entry = readdir(entries))) {
        if (!is_temporary(entry->d_name, prefix)) continue;
        struct stat st;
        if (fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) ||
            unlinkat(directory, entry->d_name, 0)) { bad = 1; break; }
    }
    closedir(entries);
    return bad || fsync(directory) ? -1 : 0;
}

int ota_state_initialize(const char *path, uint64_t accepted, char *error, size_t size)
{
    int directory = private_directory(path, error, size);
    if (directory < 0) return -1;
    int lock = lock_directory(directory, error, size);
    if (lock < 0) { close(directory); return -1; }
    char value[32];
    int length = snprintf(value, sizeof value, "%" PRIu64 "\n", accepted);
    int rc = atomic_record(directory, "accepted", (const uint8_t *)value, (size_t)length, error, size);
    close(lock);
    close(directory);
    return rc;
}

int ota_state_staged(struct ota_state *state, int *fd, char *error, size_t size)
{
    *fd = openat(state->staging, "pending.cap", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (*fd < 0) return errno == ENOENT ? 0 : fail(error, size, "cannot open pending capsule");
    struct ota_verified staged;
    if (private_file(*fd) || ota_verify_fd(*fd, state->key, state->accepted, &staged, error, size) ||
        (state->has_pending && memcmp(staged.header, state->pending.header, OTA_HEADER_SIZE))) {
        close(*fd); *fd = -1;
        return fail(error, size, "staged capsule is unsafe, invalid or differs from journal");
    }
    return 1;
}

int ota_state_refresh(struct ota_state *state, char *error, size_t size)
{
    uint8_t value[OTA_HEADER_SIZE + 1];
    size_t length;
    if (read_private(state->directory, "accepted", value, 21, &length) ||
        length < 2 || value[length - 1] != '\n')
        return fail(error, size, "accepted sequence is missing or unsafe");
    for (size_t i = 0; i + 1 < length; ++i)
        if (value[i] < '0' || value[i] > '9')
            return fail(error, size, "accepted sequence contains nondecimal bytes");
    value[length - 1] = 0;
    if (ota_parse_u64((const char *)value, &state->accepted))
        return fail(error, size, "accepted sequence is malformed");
    int rc = read_private(state->directory, "pending", value, OTA_HEADER_SIZE, &length);
    if (rc < 0) return fail(error, size, "pending journal is unsafe");
    state->has_pending = rc == 0;
    if (state->has_pending && (length != OTA_HEADER_SIZE ||
        ota_verify_header(value, state->key, state->accepted, &state->pending, error, size)))
        return fail(error, size, "pending journal authentication/sequence failed");
    int fd;
    rc = ota_state_staged(state, &fd, error, size);
    if (rc < 0) return -1;
    if (rc == 1) {
        if (!state->has_pending) {
            struct ota_verified staged;
            rc = ota_verify_fd(fd, state->key, state->accepted, &staged, error, size);
            if (!rc) rc = atomic_record(state->directory, "pending", staged.header,
                                         OTA_HEADER_SIZE, error, size);
            if (!rc) { state->pending = staged; state->has_pending = 1; }
        }
        close(fd);
        if (rc < 0) return -1;
    }
    return 0;
}

int ota_state_open(struct ota_state *state, const char *path, const char *staging,
                   const uint8_t key[32], char *error, size_t size)
{
    memset(state, 0, sizeof *state);
    state->directory = state->staging = state->lock = -1;
    memcpy(state->key, key, 32);
    state->directory = private_directory(path, error, size);
    if (state->directory < 0) goto failed;
    state->lock = lock_directory(state->directory, error, size);
    if (state->lock < 0) goto failed;
    state->staging = private_directory(staging, error, size);
    if (state->staging < 0) goto failed;
    struct stat a, b;
    if (fstat(state->directory, &a) || fstat(state->staging, &b) ||
        (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) {
        fail(error, size, "state and staging must be different private directories"); goto failed;
    }
    if (cleanup_temporary(state->directory, "journal") || cleanup_temporary(state->staging, "upload")) {
        fail(error, size, "cannot clean unsafe/incomplete temporary files"); goto failed;
    }
    if (ota_state_refresh(state, error, size)) goto failed;
    return 0;
failed:
    ota_state_close(state);
    return -1;
}

void ota_state_close(struct ota_state *state)
{
    if (state->lock >= 0) close(state->lock);
    if (state->staging >= 0) close(state->staging);
    if (state->directory >= 0) close(state->directory);
    state->directory = state->staging = state->lock = -1;
}

int ota_state_upload(struct ota_state *state, char *name, size_t name_size,
                     char *error, size_t size)
{
    int fd = new_temporary(state->staging, "upload", name, name_size);
    if (fd < 0) return fail(error, size, "cannot create private upload file");
    return fd;
}

int ota_state_publish(struct ota_state *state, int fd, const char *temporary,
                      const struct ota_verified *verified, int *resumed,
                      char *error, size_t size)
{
    if (ota_state_refresh(state, error, size)) return -1;
    if (verified->sequence <= state->accepted || (state->has_pending &&
        memcmp(verified->header, state->pending.header, OTA_HEADER_SIZE)))
        return fail(error, size, "accepted/pending state conflicts with upload");
    *resumed = state->has_pending;
    struct stat opened, named, existing;
    if (fstat(fd, &opened) || private_file(fd) ||
        fstatat(state->staging, temporary, &named, AT_SYMLINK_NOFOLLOW) ||
        opened.st_ino != named.st_ino || opened.st_dev != named.st_dev ||
        !fstatat(state->staging, "pending.cap", &existing, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return fail(error, size, "upload identity changed or stage already exists");
    if (fsync(fd) || renameat(state->staging, temporary, state->staging, "pending.cap") ||
        fsync(state->staging)) return fail(error, size, "stage publication/sync failed");
    if (!state->has_pending && atomic_record(state->directory, "pending", verified->header,
                                              OTA_HEADER_SIZE, error, size)) return -1;
    state->pending = *verified;
    state->has_pending = 1;
    return 0;
}
