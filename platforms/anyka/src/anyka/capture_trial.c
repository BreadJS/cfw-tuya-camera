/* SPDX-License-Identifier: MIT
 * Experimental single-owner capture trial. No flash writes or cloud startup.
 * Build only for the fail-closed SDK host, after reviewing the exact target ABI.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "sdk_api.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TRIAL_AU_LIMIT (512u * 1024u)
#define TRIAL_FILE_LIMIT (8u * 1024u * 1024u)
#define TRIAL_FRAMES 45u
#define TRIAL_SECONDS 15u
#define VI_POOL (13u * 1024u * 1024u)
#define VENC_POOL (10u * 1024u * 1024u)
#define PROFILE "/etc/config/isp_mis2006_mipi_2lane_h3b.conf"
static unsigned hardware_attempted;

static uint64_t monotonic_us(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static int stage(const char *name, int rc)
{
    printf("ANYKA_CAPTURE_STAGE name=%s rc=%d monotonic_us=%llu\n", name, rc,
           (unsigned long long)monotonic_us());
    fflush(stdout);
    return rc;
}

static void hold(const char *name, int rc)
{
    printf("ANYKA_CAPTURE_HOLD stage=%s rc=%d resources_retained=yes cold_reboot_required=yes\n", name, rc);
    fflush(stdout);
    for (;;) pause();
}

static int checked(const char *name, int rc)
{
    stage(name, rc);
    if (rc && hardware_attempted) hold(name, rc);
    return rc;
}

/* Refuse the real vendor executable: ignored/failed preload must never fall
 * through into a second camera owner. The host copy has this exact exit125 ARM
 * stub at its original main. Original flash/running executable is not patched.
 */
static int safe_sdk_host(void)
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
static int exclusive_owner(void)
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
static int dma_budget(void)
{
    unsigned char reg[8];
    int fd = open("/proc/device-tree/reserved-memory/dma_reserved@0x81400000/reg", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t n = read(fd, reg, sizeof reg); close(fd);
    if (n != sizeof reg || be32(reg) != UINT32_C(0x81400000) ||
        be32(reg + 4) != 24u * 1024u * 1024u || VI_POOL + VENC_POOL > be32(reg + 4)) return -ERANGE;
    return 0;
}

static int sd_output(void)
{
    FILE *mounts = fopen("/proc/mounts", "r"); if (!mounts) return -errno;
    char line[1024], source[256], point[256], type[64]; int found = 0;
    while (fgets(line, sizeof line, mounts))
        if (sscanf(line, "%255s %255s %63s", source, point, type) == 3 &&
            !strcmp(source, "/dev/mmcblk0p1") && !strcmp(point, "/mnt") && !strcmp(type, "vfat")) found = 1;
    fclose(mounts); if (!found) return -ENODEV;
    int mountfd = open("/mnt", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (mountfd < 0) return -errno;
    struct statfs fs; int rc = -EPERM, folder = -1, fd = -1;
    if (fstatfs(mountfd, &fs) || fs.f_type != 0x4d44) goto done;
    if (mkdirat(mountfd, "camerhack-ptz-sdk-trial", 0755) && errno != EEXIST) { rc = -errno; goto done; }
    folder = openat(mountfd, "camerhack-ptz-sdk-trial", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder < 0) { rc = -errno; goto done; }
    if (fstatfs(folder, &fs) || fs.f_type != 0x4d44) goto done;
    fd = openat(folder, "capture.h264", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) rc = -errno;
done:
    if (folder >= 0) close(folder);
    close(mountfd); return fd >= 0 ? fd : rc;
}

static int readable(uint32_t address, uint32_t bytes)
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

static int annex_b(const uint8_t *data, size_t bytes, unsigned *seen)
{
    unsigned nals = 0;
    for (size_t i = 0; i + 3 < bytes; i++) {
        size_t prefix = 0;
        if (!data[i] && !data[i + 1] && data[i + 2] == 1) prefix = 3;
        else if (i + 4 < bytes && !data[i] && !data[i + 1] && !data[i + 2] && data[i + 3] == 1) prefix = 4;
        if (!prefix) { if (!nals && data[i]) return -EBADMSG; continue; }
        unsigned kind = data[i + prefix] & 31;
        if ((data[i + prefix] & 128) || !kind || kind > 23) return -EBADMSG;
        if (kind == 7) *seen |= 1;
        if (kind == 8) *seen |= 2;
        if (kind == 5) *seen |= 4;
        nals++; i += prefix;
    }
    return nals ? 0 : -EBADMSG;
}

static int write_all(int fd, const uint8_t *data, size_t bytes)
{
    while (bytes) {
        ssize_t n = write(fd, data, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        data += n; bytes -= (size_t)n;
    }
    return 0;
}

int camera_anyka_capture_trial(void)
{
    const char *enabled = getenv("CAMERA_ANYKA_CAPTURE_TRIAL");
    if (!enabled || strcmp(enabled, "1") || geteuid() != 0) return stage("explicit-trial-gate", -EPERM);
    if (checked("fail-closed-SDK-host", safe_sdk_host())) return -EPERM;
    if (checked("exclusive-owner", exclusive_owner())) return -EBUSY;
    if (checked("DMA-reserved-budget", dma_budget())) return -ERANGE;
    struct stat profile;
    if (stat(PROFILE, &profile) || !S_ISREG(profile.st_mode) || profile.st_size < 1024) return stage("sensor-profile", -ENOENT);
    struct anyka_sdk_api api; char error[128];
    int rc = anyka_sdk_bind_default(&api, error, sizeof error);
    if (checked("SDK-binding", rc)) { printf("ANYKA_CAPTURE_BIND_ERROR %s\n", error); fflush(stdout); return rc; }
    int output = sd_output(); if (output < 0) return stage("SD-output", output);
    uint8_t *copy = malloc(TRIAL_AU_LIMIT);
    if (!copy) { close(output); return stage("owned-AU-buffer", -ENOMEM); }
    if (checked("exclusive-owner-recheck", exclusive_owner())) { close(output); free(copy); return -EBUSY; }
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    hardware_attempted = 1;
    checked("mem-init-two-arguments", api.mem_init(0, 0));
    checked("VENC-pool-10MiB", api.dma_pool_create(ANYKA_DMA_MODULE_VENC, VENC_POOL));
    checked("VI-pool-13MiB", api.dma_pool_create(ANYKA_DMA_MODULE_VI, VI_POOL));
    checked("DMA-activate", api.dma_pool_activate());
    checked("VI-open", api.vi_open(0));
    checked("MIS2006-profile", api.vi_load_sensor_config(0, PROFILE));
    uint32_t dimensions[2] = {0};
    checked("sensor-resolution", api.vi_get_sensor_resolution(0, dimensions));
    if (dimensions[0] != 1920 || dimensions[1] != 1080) hold("unexpected-sensor-resolution", -ERANGE);
    struct anyka_vi_device_attr device = {0};
    device.crop_width = 1920; device.crop_height = 1080;
    device.main_max_width = 1920; device.main_max_height = 1080;
    device.fps = 15; device.sub_max_width = 640; device.sub_max_height = 360;
    checked("VI-device-attributes", api.vi_set_device_attr(0, &device));
    struct anyka_vi_channel_attr channel = {0, 15, 1920, 1080, 2};
    checked("VI-main-channel", api.vi_set_channel_attr(0, &channel));
    /* Exact ak_vi_enable_dev walks both physical VI channels and rejects
     * a missing subchannel even when only main is consumed by VENC. */
    struct anyka_vi_channel_attr subchannel = {1, 15, 640, 360, 2};
    checked("VI-required-subchannel", api.vi_set_channel_attr(1, &subchannel));
    checked("VI-device-enable", api.vi_enable_device(0));
    checked("VI-main-enable", api.vi_enable_channel(0));
    struct anyka_encode_param encode = {0};
    encode.width = 1920; encode.height = 1080; encode.fps = 15; encode.gop_frames = 30;
    encode.target_kbps = 512; encode.maximum_kbps = 1024; encode.profile = 0;
    encode.rate_control = 1; encode.initial_qp = 35; encode.minimum_qp = 25; encode.maximum_qp = 45;
    encode.chroma_mode = 1; encode.encoder_level = 50;
    int32_t handle = -1;
    checked("H264-main-encoder", api.venc_open(&encode, &handle));
    if (handle < 0 || handle > 7) hold("unexpected-encoder-handle", -ERANGE);
    struct anyka_channel source = {ANYKA_DMA_MODULE_VI, 0}, destination = {ANYKA_DMA_MODULE_VENC, handle};
    struct anyka_channel_attr binding = {15, 2};
    checked("VI-to-VENC-bind", api.video_bind_channel(&source, &destination, &binding));
    checked("VENC-destination-active", api.video_set_destination_active(&destination, 1));
    uint64_t start = monotonic_us(), total = 0; unsigned frames = 0, seen = 0;
    if (!start) hold("monotonic-clock", -EIO);
    while (frames < TRIAL_FRAMES && monotonic_us() - start < TRIAL_SECONDS * UINT64_C(1000000)) {
        struct anyka_video_stream stream = {0};
        rc = api.video_get_stream(&destination, &stream);
        if ((uint32_t)rc == UINT32_C(0x1a00000a)) { usleep(10000); continue; }
        if (rc) hold("get-stream", rc);
        /* A successful get transfers one SDK-owned buffer. Preserve its
         * descriptor unmodified and release exactly once on every branch. */
        int copy_error = 0;
        if (!stream.bytes || stream.bytes > TRIAL_AU_LIMIT || !readable(stream.data_address, stream.bytes)) copy_error = -EFAULT;
        else memcpy(copy, (const void *)(uintptr_t)stream.data_address, stream.bytes);
        uint32_t bytes = stream.bytes; uint64_t timestamp = stream.source_timestamp; int32_t kind = stream.frame_type;
        rc = api.venc_release_stream(handle, &stream);
        if (rc) hold("release-stream-once", rc);
        if (copy_error) hold("stream-bounds", copy_error);
        if ((rc = annex_b(copy, bytes, &seen))) hold("H264-Annex-B", rc);
        if (total + bytes > TRIAL_FILE_LIMIT) hold("capture-file-bound", -EFBIG);
        if ((rc = write_all(output, copy, bytes))) hold("SD-write", rc);
        total += bytes; frames++;
        printf("ANYKA_CAPTURE_FRAME index=%u bytes=%u source_timestamp=%llu frame_type=%d monotonic_us=%llu\n",
               frames, bytes, (unsigned long long)timestamp, kind, (unsigned long long)monotonic_us());
        fflush(stdout);
    }
    if (fsync(output)) hold("SD-fsync", -errno);
    close(output);
    printf("ANYKA_CAPTURE_RESULT frames=%u bytes=%llu elapsed_us=%llu headers=%u resources_retained=yes\n",
           frames, (unsigned long long)total, (unsigned long long)(monotonic_us() - start), seen);
    fflush(stdout);
    if (frames != TRIAL_FRAMES || seen != 7) hold("capture-incomplete", -ETIMEDOUT);
    hold("capture-complete", 0);
    return 0;
}

#ifdef CAMERA_ANYKA_CAPTURE_TRIAL_CONSTRUCTOR
__attribute__((constructor)) static void capture_constructor(void)
{
    int rc = camera_anyka_capture_trial();
    fflush(stdout);
    _exit(rc ? 125 : 0); /* Never fall through into vendor main. */
}
#endif
