#ifndef CAMERA_ANYKA_SDK_HOST_GUARD_H
#define CAMERA_ANYKA_SDK_HOST_GUARD_H

/* Shape/stub gate for the exact reviewed SDK-host layout. The preparer pins the
 * complete input and output hashes; this lightweight runtime check additionally
 * prevents accidental use of an unpatched stock executable. Define _GNU_SOURCE
 * and _FILE_OFFSET_BITS=64 before libc headers in each consuming source file.
 * Reading /proc/self/exe never opens a camera device or calls vendor code. */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int camera_sdk_host_read(int fd, void *buffer, size_t bytes, uint64_t offset)
{
    unsigned char *out = buffer;
    while (bytes) {
        ssize_t count = pread(fd, out, bytes, (off_t)offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -errno;
        if (!count) return -EPERM;
        out += count;
        bytes -= (size_t)count;
        offset += (size_t)count;
    }
    return 0;
}

static inline int camera_sdk_host_guard_fd(int fd)
{
    static const uint32_t expected[4] = {
        UINT32_C(0xe3a0007d), UINT32_C(0xe3a07001),
        UINT32_C(0xef000000), UINT32_C(0xeafffffe)
    };
    const uint64_t main_address = UINT32_C(0x9f8b0);
    struct stat info;
    Elf32_Ehdr eh;
    if (fstat(fd, &info)) return -errno;
    if (!S_ISREG(info.st_mode) || info.st_size < (off_t)sizeof eh) return -EPERM;
    uint64_t file_bytes = (uint64_t)info.st_size;
    int rc = camera_sdk_host_read(fd, &eh, sizeof eh, 0);
    if (rc) return rc;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_ident[EI_VERSION] != EV_CURRENT || eh.e_version != EV_CURRENT ||
        eh.e_machine != EM_ARM || eh.e_type != ET_EXEC ||
        eh.e_entry != UINT32_C(0x5f970) || eh.e_ehsize != sizeof eh ||
        (eh.e_flags & UINT32_C(0xff000000)) != UINT32_C(0x05000000) ||
        (eh.e_flags & UINT32_C(0x400)) || !eh.e_phnum || eh.e_phnum > 32 ||
        eh.e_phentsize != sizeof(Elf32_Phdr) || eh.e_phoff < sizeof eh ||
        (uint64_t)eh.e_phoff + (uint64_t)eh.e_phnum * sizeof(Elf32_Phdr) > file_bytes)
        return -EPERM;
    unsigned matches = 0;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf32_Phdr ph;
        rc = camera_sdk_host_read(fd, &ph, sizeof ph,
                                  (uint64_t)eh.e_phoff + (uint64_t)i * sizeof ph);
        if (rc) return rc;
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_filesz > ph.p_memsz || (uint64_t)ph.p_offset + ph.p_filesz > file_bytes)
            return -EPERM;
        if (!(ph.p_flags & PF_X) || main_address < ph.p_vaddr ||
            main_address + sizeof expected > (uint64_t)ph.p_vaddr + ph.p_filesz)
            continue;
        uint64_t offset = (uint64_t)ph.p_offset + main_address - ph.p_vaddr;
        if (offset != UINT32_C(0x8f8b0)) return -EPERM;
        uint32_t actual[4];
        rc = camera_sdk_host_read(fd, actual, sizeof actual, offset);
        if (rc) return rc;
        if (memcmp(actual, expected, sizeof expected)) return -EPERM;
        ++matches;
    }
    return matches == 1 ? 0 : -EPERM;
}

static inline int camera_sdk_host_guard(void)
{
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = camera_sdk_host_guard_fd(fd);
    close(fd);
    return rc;
}

#endif
