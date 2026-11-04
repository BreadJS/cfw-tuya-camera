/* SPDX-License-Identifier: MIT
 * Read-only preflight guards shared by experimental Anyka SDK-host trials.
 * Copied from the capture guard already reviewed/tested for this board.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "trial_guard.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int anyka_trial_safe_sdk_host(void)
{
    static const uint32_t stub[4] = {0xe3a0007d, 0xe3a07001, 0xef000000, 0xeafffffe};
    Elf32_Ehdr eh; Elf32_Phdr ph; uint32_t actual[4]; int rc = -EPERM;
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_machine != EM_ARM || eh.e_type != ET_EXEC || eh.e_phnum > 32 ||
        eh.e_phentsize != sizeof ph) goto done;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        if (pread(fd, &ph, sizeof ph, eh.e_phoff + (off_t)i * sizeof ph) != sizeof ph) goto done;
        uint64_t main_address = UINT32_C(0x9f8b0);
        if (ph.p_type != PT_LOAD || main_address < ph.p_vaddr ||
            main_address + sizeof stub > (uint64_t)ph.p_vaddr + ph.p_filesz) continue;
        off_t at = (off_t)ph.p_offset + (off_t)(main_address - ph.p_vaddr);
        if (pread(fd, actual, sizeof actual, at) == sizeof actual && !memcmp(actual, stub, sizeof stub)) rc = 0;
        break;
    }
done:
    close(fd); return rc;
}

static int numeric(const char *name)
{ if (!*name) return 0; while (*name) { if (*name < '0' || *name++ > '9') return 0; } return 1; }
static int media_device(const char *path)
{
    return !strncmp(path, "/dev/video-", 11) || !strncmp(path, "/dev/isp-", 9) ||
           !strncmp(path, "/dev/uio", 8) || !strcmp(path, "/dev/ion");
}
int anyka_trial_exclusive_owner(void)
{
    DIR *proc = opendir("/proc"); if (!proc) return -errno;
    int rc = 0; struct dirent *entry;
    while ((entry = readdir(proc))) {
        if (!numeric(entry->d_name) || strtoul(entry->d_name, NULL, 10) == (unsigned long)getpid()) continue;
        char directory[128];
        if (snprintf(directory, sizeof directory, "/proc/%s/fd", entry->d_name) >= (int)sizeof directory) { rc = -EOVERFLOW; break; }
        DIR *fds = opendir(directory);
        if (!fds) { if (errno == ENOENT || errno == ESRCH) continue; rc = -errno; break; }
        struct dirent *fd;
        while ((fd = readdir(fds))) {
            if (!numeric(fd->d_name)) continue;
            char path[512], target[512];
            int n = snprintf(path, sizeof path, "%s/%s", directory, fd->d_name);
            if (n < 0 || n >= (int)sizeof path) { rc = -EOVERFLOW; break; }
            ssize_t got = readlink(path, target, sizeof target - 1);
            if (got < 0) { if (errno == ENOENT || errno == ESRCH) continue; rc = -errno; break; }
            target[got] = 0;
            if (media_device(target)) {
                printf("ANYKA_CAPTURE_OWNER_CONFLICT pid=%s device=%s\n", entry->d_name, target);
                fflush(stdout); rc = -EBUSY; break;
            }
        }
        closedir(fds); if (rc) break;
    }
    closedir(proc); return rc;
}

static uint32_t be32(const unsigned char *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
int anyka_trial_dma_budget(uint32_t vi_pool, uint32_t venc_pool)
{
    unsigned char reg[8];
    int fd = open("/proc/device-tree/reserved-memory/dma_reserved@0x81400000/reg", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t n = read(fd, reg, sizeof reg); close(fd);
    if (n != sizeof reg || be32(reg) != UINT32_C(0x81400000) ||
        be32(reg + 4) != 24u * 1024u * 1024u || !vi_pool || !venc_pool || (uint64_t)vi_pool + venc_pool > be32(reg + 4)) return -ERANGE;
    return 0;
}

int anyka_trial_readable(uint32_t address, uint32_t bytes)
{
    uint64_t end = (uint64_t)address + bytes;
    if (!address || !bytes || end > UINT32_MAX) return 0;
    FILE *maps = fopen("/proc/self/maps", "r"); if (!maps) return 0;
    char line[512], permissions[8]; unsigned long low, high; int valid = 0;
    while (fgets(line, sizeof line, maps))
        if (sscanf(line, "%lx-%lx %7s", &low, &high, permissions) == 3 && permissions[0] == 'r' &&
            address >= low && end <= high) { valid = 1; break; }
    fclose(maps); return valid;
}

