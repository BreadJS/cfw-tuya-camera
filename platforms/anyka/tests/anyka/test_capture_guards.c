#include "../../src/anyka/capture_trial.c"
#include <assert.h>
int main(void)
{
    assert(!numeric("") && !numeric("-1") && !numeric("1x"));
    assert(numeric("123"));
    assert(media_device("/dev/video-0-0") && media_device("/dev/isp-param-0"));
    assert(media_device("/dev/uio0") && media_device("/dev/ion"));
    assert(!media_device("/dev/watchdog") && !media_device("/dev/motor0"));
    assert(!media_device("/tmp/not-a-video-device"));
    const uint8_t valid[] = {0,0,0,1,0x67,0x80,0,0,1,0x68,0x80,0,0,1,0x65,0x80};
    unsigned seen = 0; assert(!annex_b(valid, sizeof valid, &seen) && seen == 7);
    const uint8_t bad[] = {1,0,0,0,1,0x65,0x80};
    assert(annex_b(bad, sizeof bad, &seen) == -EBADMSG);
    const uint8_t invalid_nal[] = {0,0,0,1,0xe5,0x80};
    assert(annex_b(invalid_nal, sizeof invalid_nal, &seen) == -EBADMSG);
    assert(!hardware_attempted);
    unsetenv("CAMERA_ANYKA_CAPTURE_TRIAL");
    assert(camera_anyka_capture_trial() == -EPERM);
    assert(!hardware_attempted);
    /* The development host cannot pass the fail-closed ARM SDKhost check. */
    assert(safe_sdk_host() == -EPERM);
    puts("Anyka capture guards: explicit gate, host refusal, device filters and Annex-B parser PASS");
    return 0;
}
