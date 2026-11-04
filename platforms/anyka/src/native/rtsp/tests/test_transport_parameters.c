/* Exercise production queues with deterministic interrupted/partial syscalls. */
#define send interrupted_send
#define recv interrupted_recv
#include "../camera_rtsp.c"
#include <assert.h>
static unsigned step;static uint8_t wire[1024];static size_t wire_size;
ssize_t interrupted_send(int fd,const void *data,size_t size,int flags)
{
    (void)fd;(void)flags;unsigned current=step++;
    if(current==1||current==2){errno=current==1?EINTR:EAGAIN;return -1;}
    size_t n=current==0?3:size;assert(wire_size+n<=sizeof wire);
    memcpy(wire+wire_size,data,n);wire_size+=n;return (ssize_t)n;
}
ssize_t interrupted_recv(int fd,void *data,size_t size,int flags)
{(void)fd;(void)data;(void)size;(void)flags;errno=EINTR;return -1;}
static void interrupted_packet(void)
{
    struct client c={0};struct server s={0};c.tx=calloc(1,TX_CAP);assert(c.tx);
    const uint8_t packet[]={36,0,0,4,0x80,0x80,0,1},tail[]={36,1,0,1,0xaa};
    assert(!append(&c,packet,sizeof packet));assert(!flush_client(&c,1));assert(c.sent==3);
    assert(!flush_client(&c,2)&&c.sent==3&&c.queued==sizeof packet);
    assert(!flush_client(&c,3)&&c.sent==3&&c.queued==sizeof packet);
    assert(!receive_requests(&s,&c)&&!c.received);
    assert(!append(&c,tail,sizeof tail)&&!c.sent&&c.queued==sizeof packet-3+sizeof tail);
    assert(!flush_client(&c,4)&&!c.sent&&!c.queued);
    assert(wire_size==sizeof packet+sizeof tail&&!memcmp(wire,packet,sizeof packet)&&!memcmp(wire+sizeof packet,tail,sizeof tail));free(c.tx);
}
static void parameter_refresh(void)
{
    struct server s={0};const uint8_t sps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xde,0x40,0xfa,0x01,0x77,0x7f,0xff,0x02,0x80},pps[]={0x68,0x80};
    memcpy(s.sps,sps,sizeof sps);s.sps_size=sizeof sps;memcpy(s.pps,pps,sizeof pps);s.pps_size=sizeof pps;
    const uint8_t update[]={0,0,1,0x68,0xc0,0,0,1,0x41,0x80};
    struct camera_media_frame f={.data=update,.size=sizeof update,.pts_us=1000000};
    assert(!video_parameters(&s,&f)&&s.pps_size==2&&s.pps[1]==0xc0&&!strcmp(s.pps64,"aMA="));
    const uint8_t changed_sps[]={0,0,1,0x67,77,1,40};f.data=changed_sps;f.size=sizeof changed_sps;assert(video_parameters(&s,&f));
    /* An IDR-only resync still receives newest PPS even if update AU dropped. */
    struct camera_media_provider provider={0};s.provider=&provider;
    struct client c={0};c.tx=calloc(1,TX_CAP);assert(c.tx);c.playing=c.need_idr=c.clock_started=c.video.setup=1;
    const uint8_t idr[]={0,0,1,0x65,0x80};f.data=idr;f.size=sizeof idr;f.keyframe=1;
    assert(!video_frame(&s,&c,&f));size_t at=0;unsigned count=0;
    while(at<c.queued){assert(c.tx[at]==36);size_t n=((size_t)c.tx[at+2]<<8)|c.tx[at+3];
        const uint8_t *payload=c.tx+at+16;
        if(count==0)assert(!memcmp(payload,sps,sizeof sps));
        if(count==1)assert(!memcmp(payload,s.pps,s.pps_size));
        assert((c.tx[at+5]&128)==(count==2?128:0));at+=4+n;count++;
    }
    assert(count==3);free(c.tx);
    const uint8_t hrdsps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xda,0x40,0x2e,0xe0,0x17,0x77,0xff,0xf0,0x28};
    uint8_t au[4+sizeof hrdsps+5]={0,0,0,1};memcpy(au+4,hrdsps,sizeof hrdsps);
    memcpy(au+4+sizeof hrdsps,"\0\0\1\x68\xd0",5);
    f.data=au;f.size=sizeof au;
    assert(!video_parameters(&s,&f)&&s.sps_size==sizeof hrdsps&&!memcmp(s.sps,hrdsps,sizeof hrdsps)&&s.pps[1]==0xd0);
    char encoded[348];assert(!base64(encoded,sizeof encoded,hrdsps,sizeof hrdsps)&&!strcmp(encoded,s.sps64));
    assert(!strcmp(s.pps64,"aNA="));
    au[5]=66;assert(video_parameters(&s,&f)); /* forbidden profile change */
    au[5]=0x4d;au[10]^=2;assert(video_parameters(&s,&f)); /* forbidden width change */
    assert(!memcmp(s.sps,hrdsps,sizeof hrdsps));
}
int main(void){interrupted_packet();parameter_refresh();puts("PASS interrupted partial RTP preservation, PPS SDP refresh and IDR parameter recovery");return 0;}
