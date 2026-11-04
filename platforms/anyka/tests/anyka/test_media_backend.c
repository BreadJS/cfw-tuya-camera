#include "../../src/anyka/media_backend.c"
#include <assert.h>
#include <sys/mman.h>

static uint64_t test_now_us = 20000000;
uint64_t camera_monotonic_us(void) { return test_now_us; }
static unsigned gets, releases, keyframe_requests;
void camera_request_keyframe(void){keyframe_requests++;}
static int get_error, release_error;
static struct anyka_video_stream input;
static uint8_t *loan;
static int get_stream(struct anyka_channel *channel, struct anyka_video_stream *out)
{ (void)channel; gets++; *out = input; return get_error; }
static int release_stream(int32_t handle, struct anyka_video_stream *stream)
{
    (void)handle; releases++;
    if (loan && stream->bytes <= 4096) memset(loan, 0, stream->bytes);
    stream->data_address = 0;
    return release_error;
}
static const uint8_t frame[] = {0,0,0,1,0x67,0x42,0,0x1e,0xf4,0xf2,
    0,0,0,1,0x68,0xce,0,0,0,1,0x65,0xb8};
/* Captured Main-profile 1920x1080 SPS: VCL HRD scale7/value124 = 1024000.
 * The variant keeps every decoder-critical bit and uses scale6/value374 for
 * exactly 1536000 bps; Exp-Golomb growth also exercises shifted trailing bits. */
static const uint8_t captured_sps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xde,0x40,0xfa,0x01,0x77,0x7f,0xff,0x02,0x80};
static const uint8_t bitrate_sps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xda,0x40,0x2e,0xe0,0x17,0x77,0xff,0xf0,0x28};

static void parameter_checks(void)
{
    struct anyka_media_backend b = {0}; unsigned key, vcl;
    assert(!inspect_au(&b, frame, sizeof frame, &key, &vcl) && key && vcl);
    assert(b.sps_bytes == 6 && b.pps_bytes == 2);
    assert(!inspect_au(&b, frame, sizeof frame, &key, &vcl));
    uint8_t changed[sizeof frame]; memcpy(changed, frame, sizeof frame); changed[7] ^= 1; /* level */
    assert(inspect_au(&b, changed, sizeof changed, &key, &vcl) == -EPROTO);
    uint8_t repeated[sizeof frame * 2]; memcpy(repeated, frame, sizeof frame);
    memcpy(repeated + sizeof frame, changed, sizeof changed);
    assert(inspect_au(&b, repeated, sizeof repeated, &key, &vcl) == -EPROTO);
    uint8_t changed_pps[sizeof frame];memcpy(changed_pps,frame,sizeof frame);changed_pps[15]=0xcf;
    assert(!inspect_au(&b,changed_pps,sizeof changed_pps,&key,&vcl)&&key&&vcl&&b.parameter_changes==1&&keyframe_requests==1);
    assert(!inspect_au(&b,changed_pps,sizeof changed_pps,&key,&vcl)&&b.parameter_changes==1&&keyframe_requests==1);
    changed_pps[15]=0xa0; /* PPS0 refers to SPS1, which was never supplied. */
    assert(inspect_au(&b,changed_pps,sizeof changed_pps,&key,&vcl)==-EPROTO);
    changed_pps[15]=0; /* Unbounded/truncated Exp-Golomb ID. */
    assert(inspect_au(&b,changed_pps,sizeof changed_pps,&key,&vcl)==-EBADMSG);
    uint8_t bad[] = {0,0,0,1,0xe5,0x80};
    assert(inspect_au(&b, bad, sizeof bad, &key, &vcl) == -EBADMSG);
    assert(inspect_au(&b, frame + 2, sizeof frame - 2, &key, &vcl) == -EBADMSG);
    assert(inspect_au(&b, frame, 0, &key, &vcl) == -EMSGSIZE);
    uint8_t large[CAMERA_RTSP_MAX_PARAMETER_SET + 5];
    memset(large, 0x80, sizeof large); large[0] = large[1] = large[2] = 0; large[3] = 1; large[4] = 0x67;
    memset(&b, 0, sizeof b);
    assert(inspect_au(&b, large, sizeof large, &key, &vcl) == -EMSGSIZE);
}

static void captured_parameters(void)
{
    /* Actual pre-change SDK output from the parent's controlled RTSP capture.
     * NAL ref_idc1 (27/28), rather than the synthetic 67/68 fixture. */
    static const uint8_t pps[]={0x28,0xee,0x3c,0x80};
    struct anyka_media_backend b={0};unsigned id=99;
    assert(!sps_id(captured_sps,sizeof captured_sps,&id)&&id==0);
    assert(!parameter(&b,7,captured_sps,sizeof captured_sps)&&!parameter(&b,8,pps,sizeof pps));
    assert(parameter_crc((const uint8_t *)"123456789",9)==UINT32_C(0xcbf43926));

    unsigned requests=keyframe_requests;
    assert(!camera_h264_sps_compatible(captured_sps,sizeof captured_sps,bitrate_sps,sizeof bitrate_sps));
    assert(!parameter(&b,7,bitrate_sps,sizeof bitrate_sps));
    assert(b.parameter_changes==1&&keyframe_requests==requests+1);
    assert(b.sps_bytes==sizeof bitrate_sps&&!memcmp(b.sps,bitrate_sps,sizeof bitrate_sps));
    assert(!parameter(&b,7,bitrate_sps,sizeof bitrate_sps)&&keyframe_requests==requests+1);

    uint8_t changed[sizeof bitrate_sps];
    memcpy(changed,bitrate_sps,sizeof changed);changed[1]=0x42; /* valid Baseline syntax, changed profile */
    assert(parameter(&b,7,changed,sizeof changed)==-EPROTO);
    memcpy(changed,bitrate_sps,sizeof changed);changed[6]^=2; /* width 119 -> 123 macroblocks */
    assert(parameter(&b,7,changed,sizeof changed)==-EPROTO);
    memcpy(changed,bitrate_sps,sizeof changed);changed[9]^=1; /* frame crop bottom 4 -> 6 */
    assert(parameter(&b,7,changed,sizeof changed)==-EPROTO);
    memcpy(changed,bitrate_sps,sizeof changed);changed[5]^=0x20; /* max_num_ref_frames 1 -> 2 */
    assert(parameter(&b,7,changed,sizeof changed)==-EPROTO);
    assert(b.parameter_changes==1&&keyframe_requests==requests+1);
    /* Invalid VUI, HRD/trailing truncations, broken emulation prevention. */
    for(size_t n=1;n<sizeof bitrate_sps;n++)
        assert(camera_h264_sps_compatible(captured_sps,sizeof captured_sps,bitrate_sps,n)<0);
    memcpy(changed,bitrate_sps,sizeof changed);changed[18]=4;
    assert(camera_h264_sps_compatible(captured_sps,sizeof captured_sps,changed,sizeof changed)==-EBADMSG);
    memcpy(changed,bitrate_sps,sizeof changed);changed[10]^=0x10; /* reserved aspect_ratio_idc 129 */
    assert(camera_h264_sps_compatible(captured_sps,sizeof captured_sps,changed,sizeof changed)==-EBADMSG);
    memcpy(changed,bitrate_sps,sizeof changed);changed[sizeof changed-1]|=1;
    assert(camera_h264_sps_compatible(captured_sps,sizeof captured_sps,changed,sizeof changed)==-EBADMSG);
    uint8_t oversized[257];memset(oversized,0xff,sizeof oversized);
    assert(camera_h264_sps_compatible(captured_sps,sizeof captured_sps,oversized,sizeof oversized)==-EMSGSIZE);

    /* Independent libx264 one-frame encodes exercise High-profile chroma and
     * bit-depth syntax, plus VUI bitstream restrictions absent on the camera. */
    static const uint8_t high[]={0x67,0x64,0,0x0a,0xac,0xd9,0x49,0x6c,0x04,0x40,0,0,3,0,0x40,0,0,7,0x83,0xc4,0x89,0x65,0x80};
    static const uint8_t high10[]={0x67,0x6e,0,0x0a,0xa6,0xcd,0x94,0x96,0xc0,0x44,0,0,3,0,4,0,0,3,0,0x78,0x3c,0x48,0x96,0x58};
    static const uint8_t high444[]={0x67,0xf4,0,0x0a,0x91,0x9b,0x29,0x2d,0x80,0x88,0,0,3,0,8,0,0,3,0,0xf0,0x78,0x91,0x2c,0xb0};
    assert(!camera_h264_sps_compatible(high,sizeof high,high,sizeof high));
    assert(!camera_h264_sps_compatible(high10,sizeof high10,high10,sizeof high10));
    assert(!camera_h264_sps_compatible(high444,sizeof high444,high444,sizeof high444));
    assert(camera_h264_sps_compatible(high,sizeof high,high10,sizeof high10)==-EPROTO);
    assert(camera_h264_sps_compatible(high10,sizeof high10,high444,sizeof high444)==-EPROTO);
}

static void clock_checks(void)
{
    uint64_t pts = 77;
    assert(!source_clock(19990, 19900000, test_now_us, &pts) && pts == 19990000);
    assert(source_clock(19990, pts, test_now_us, &pts) == -ERANGE);
    assert(source_clock(19980, 19990000, test_now_us, &pts) == -ERANGE);
    assert(source_clock(20001, 0, test_now_us, &pts) == -ERANGE);
    assert(source_clock(1000, 0, test_now_us, &pts) == -ERANGE);
    assert(source_clock(UINT64_MAX, 0, test_now_us, &pts) == -ERANGE);
}

static void rejected_loan_release(void)
{
    struct anyka_media_backend b = {0}; b.owned = malloc(CAMERA_RTSP_MAX_AU); assert(b.owned);
    b.api.video_get_stream = get_stream; b.api.venc_release_stream = release_stream;
    input = (struct anyka_video_stream){0, 100, 19990, 0, 1};
    gets = releases = 0; get_error = release_error = 0;
    assert(pull(&b) == -1 && b.fault == -EFAULT && gets == 1 && releases == 1);
    assert(pull(&b) == -1 && gets == 1 && releases == 1);
    b.fault = 0; input.bytes = CAMERA_RTSP_MAX_AU + 1;
    assert(pull(&b) == -1 && b.fault == -EMSGSIZE && gets == 2 && releases == 2);
    b.fault = 0; input.bytes = 100; release_error = 123;
    assert(pull(&b) == -1 && b.fault == -EIO && gets == 3 && releases == 3);
    release_error = 0; free(b.owned);
}

static void owned_copy_and_provider(void)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_32BIT
    flags |= MAP_32BIT;
#endif
    void *memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE, flags, -1, 0);
    assert(memory != MAP_FAILED);
    if ((uintptr_t)memory > UINT32_MAX) {
        munmap(memory, 4096);
        puts("Low32-address lease copy unavailable on this host; pure ABI/parser/clock checks still run");
        return;
    }
    loan = memory; memcpy(loan, frame, sizeof frame);
    struct anyka_media_backend b = {0}; b.owned = malloc(CAMERA_RTSP_MAX_AU); assert(b.owned);
    b.api.video_get_stream = get_stream; b.api.venc_release_stream = release_stream;
    input = (struct anyka_video_stream){(uint32_t)(uintptr_t)loan, sizeof frame, 19990, 0, 1};
    gets = releases = 0; get_error = release_error = 0;
    assert(pull(&b) == 1 && gets == 1 && releases == 1);
    assert(!memcmp(b.owned, frame, sizeof frame) && loan[4] == 0);
    b.ready = b.first_pending = 1;
    struct camera_media_frame out;
    assert(video(&b, test_now_us, &out) == 1 && gets == 1 && releases == 1);
    assert(out.data == b.owned && out.size == sizeof frame && out.keyframe && out.pts_us == 19990000);
    assert(b.delivered == 1);
    get_error = 0x1a00000a;
    assert(video(&b, test_now_us, &out) == 0 && !out.data && gets == 2 && releases == 1);
    get_error = 0; memcpy(loan, frame, sizeof frame); input.source_timestamp = 19990;
    assert(video(&b, test_now_us, &out) == -1 && b.fault == -ERANGE && releases == 2);
    assert(video(&b, test_now_us, &out) == -1 && gets == 3 && releases == 2);
    b.fault=0;get_error=0;
    const uint8_t pps_only[]={0,0,0,1,0x68,0xcf};
    memcpy(loan,pps_only,sizeof pps_only);input.bytes=sizeof pps_only;input.source_timestamp=19991;
    assert(pull(&b)==0&&b.parameter_pending);
    const uint8_t vcl_only[]={0,0,0,1,0x65,0xb8};
    memcpy(loan,vcl_only,sizeof vcl_only);input.bytes=sizeof vcl_only;input.source_timestamp=19992;
    assert(pull(&b)==1&&!b.parameter_pending);
    assert(b.bytes==sizeof vcl_only+b.sps_bytes+b.pps_bytes+8&&b.owned[4]==0x67&&b.owned[8+b.sps_bytes]==0x68&&b.owned[9+b.sps_bytes]==0xcf);
    /* Parameter-only HRD update must survive until VCL, even if that VCL AU
     * already carries a PPS (the former PPS-only presence check lost the SPS). */
    memcpy(loan,"\0\0\0\1",4);memcpy(loan+4,captured_sps,sizeof captured_sps);
    memset(b.sps,0,sizeof b.sps);b.sps_bytes=0; /* independent source fixture */
    input.bytes=4+sizeof captured_sps;input.source_timestamp=19993;
    assert(pull(&b)==0);
    memcpy(loan,"\0\0\0\1",4);memcpy(loan+4,bitrate_sps,sizeof bitrate_sps);
    input.bytes=4+sizeof bitrate_sps;
    assert(pull(&b)==0&&b.parameter_pending);
    const uint8_t pps_vcl[]={0,0,0,1,0x68,0xcf,0,0,0,1,0x65,0xb8};
    memcpy(loan,pps_vcl,sizeof pps_vcl);input.bytes=sizeof pps_vcl;input.source_timestamp=19994;
    assert(pull(&b)==1&&!b.parameter_pending);
    assert(b.bytes==sizeof pps_vcl+sizeof bitrate_sps+b.pps_bytes+8);
    assert(!memcmp(b.owned+4,bitrate_sps,sizeof bitrate_sps));
    free(b.owned); loan = NULL; munmap(memory, 4096);
}

int main(void)
{
    parameter_checks(); captured_parameters(); clock_checks(); rejected_loan_release(); owned_copy_and_provider();
    unsetenv("CAMERA_ANYKA_RTSP_TRIAL");
    struct anyka_media_backend *b = (void *)1; struct camera_media_provider p;
    assert(anyka_media_open(&b, &p, NULL, 0) == -EPERM && !b && !p.video);
    puts("Anyka provider: all-NAL/SPS/PPS guards; source-ms clock; owned copy before exactly-once release; first IDR and sticky fault PASS");
    return 0;
}
