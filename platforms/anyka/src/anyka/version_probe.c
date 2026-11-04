#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "sdk_host_guard.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Preload constructor for the SHA-pinned SDK host. Calls only the seven
 * individually inspected constant-string getters, then _exit before main.
 * It never opens a device, initializes media, creates a thread or calls exit.
 * The host still contains proprietary/Tuya code: this probe is transitional,
 * not the standalone Tuya-free camera application. */
static int output(const char *text, size_t bytes)
{
    while (bytes) {
        ssize_t count = write(STDOUT_FILENO, text, bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        text += count;
        bytes -= (size_t)count;
    }
    return 0;
}

__attribute__((constructor)) static void probe(void)
{
    /* Only explicit non-ARM host tests may bypass the target-file gate. ARM
     * builds always enforce it, even when that development macro is supplied. */
#if !defined(CAMERA_VERSION_PROBE_HOST_TEST) || defined(__arm__)
    if (camera_sdk_host_guard()) {
        static const char refused[] = "SDK_PROBE host_guard_failed no_sdk_calls=yes\n";
        (void)output(refused, sizeof refused - 1);
        _exit(125);
    }
#endif
    static const char *const names[] = {
        "ak_vi_get_version", "ak_venc_get_version", "ak_ai_get_version",
        "ak_aenc_get_version", "ak_vpss_get_version", "ak_mem_get_version",
        "ak_app_video_get_version"
    };
    int failed = 0;
    char line[512];
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        void *address = dlsym(RTLD_DEFAULT, names[i]);
        int count;
        if (!address) {
            failed = 1;
            count = snprintf(line, sizeof line, "SDK_PROBE missing=%s\n", names[i]);
        } else {
            const char *(*version)(void);
            _Static_assert(sizeof version == sizeof address, "Function-pointer representation mismatch");
            memcpy(&version, &address, sizeof version);
            const char *text = version();
            if (!text) failed = 1;
            count = snprintf(line, sizeof line, "SDK_PROBE function=%s version=%s\n",
                             names[i], text ? text : "NULL");
        }
        if (count <= 0 || (size_t)count >= sizeof line) {
            failed = 1;
            continue;
        }
        if (output(line, (size_t)count)) _exit(3);
    }
    static const char done[] = "SDK_PROBE vendor_main_called=no probe_device_calls=none\n";
    if (output(done, sizeof done - 1)) failed = 1;
    _exit(failed ? 3 : 0);
}
