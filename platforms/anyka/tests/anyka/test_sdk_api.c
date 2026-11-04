#include "../../src/anyka/sdk_api.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct fixture { unsigned lookups, fail_at, wrong_version, null_version, hardware_calls; };
static struct fixture *active;
static const char *vi_version(void)
{ return active->null_version ? NULL : active->wrong_version ? "libplat_vi V3.4.33" : "libplat_vi V3.0.02"; }
static const char *venc_version(void) { return "libmpi_venc V1.0.29"; }
static const char *mem_version(void) { return "libplat_mem V1.3.03"; }
static const char *video_version(void) { return "libapp_video V1.0.07"; }
static int hardware(void) { active->hardware_calls++; return 0; }
static void *symbol_for_version(const char *(*fn)(void))
{ void *out; _Static_assert(sizeof out == sizeof fn, "POSIX pointers"); memcpy(&out, &fn, sizeof out); return out; }
static void *lookup(void *opaque, const char *name)
{
    struct fixture *f = opaque; active = f;
    if (++f->lookups == f->fail_at) return NULL;
    if (!strcmp(name, "ak_vi_get_version")) return symbol_for_version(vi_version);
    if (!strcmp(name, "ak_venc_get_version")) return symbol_for_version(venc_version);
    if (!strcmp(name, "ak_mem_get_version")) return symbol_for_version(mem_version);
    if (!strcmp(name, "ak_app_video_get_version")) return symbol_for_version(video_version);
    int (*fn)(void) = hardware; void *out;
    _Static_assert(sizeof out == sizeof fn, "POSIX pointers");
    memcpy(&out, &fn, sizeof out); return out;
}
static int is_zero(const void *data, size_t n)
{ const unsigned char *p = data; while (n--) if (*p++) return 0; return 1; }
int main(void)
{
    struct anyka_sdk_api api; struct fixture f = {0}; char error[128];
    memset(&api, 0xa5, sizeof api);
    assert(!anyka_sdk_bind(&api, lookup, &f, error, sizeof error));
    assert(api.version_match && !api.runtime_proven && !error[0] && !f.hardware_calls);
    assert(api.vi_open && api.vi_set_device_attr && api.venc_open && api.video_get_stream);
    unsigned count = f.lookups; assert(count > 20);
    for (unsigned i = 1; i <= count; i++) {
        f = (struct fixture){.fail_at = i}; memset(&api, 0xa5, sizeof api);
        assert(anyka_sdk_bind(&api, lookup, &f, error, sizeof error) == -ENOENT);
        assert(is_zero(&api, sizeof api) && !f.hardware_calls);
        assert(!strncmp(error, "missing SDK symbol:", 19));
    }
    f = (struct fixture){.wrong_version = 1};
    assert(anyka_sdk_bind(&api, lookup, &f, error, sizeof error) == -EPROTO);
    assert(is_zero(&api, sizeof api) && !f.hardware_calls);
    f = (struct fixture){.null_version = 1};
    assert(anyka_sdk_bind(&api, lookup, &f, error, sizeof error) == -EPROTO);
    assert(is_zero(&api, sizeof api) && !f.hardware_calls);
    assert(anyka_sdk_bind(&api, NULL, NULL, NULL, 0) == -EINVAL && is_zero(&api, sizeof api));
    assert(anyka_sdk_bind(NULL, lookup, &f, error, sizeof error) == -EINVAL);
    char tiny[3] = {'A', 'B', 'C'};
    assert(anyka_sdk_bind(NULL, lookup, &f, tiny + 1, 1) == -EINVAL);
    assert(tiny[0] == 'A' && tiny[1] == 0 && tiny[2] == 'C');
    assert(anyka_sdk_bind_default(&api, error, sizeof error) == -ENOENT);
    assert(is_zero(&api, sizeof api));
    puts("Anyka SDK bind: ABI assertions; exact versions; all missing symbols; atomic rejection; no hardware calls PASS");
    return 0;
}
