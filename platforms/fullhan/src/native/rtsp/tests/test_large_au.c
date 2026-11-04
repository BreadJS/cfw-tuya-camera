/* Actual native adapter, packetizer and server loop; no camera/driver calls. */
#define clock_gettime test_clock_gettime
#include "../camera_rtsp.c"
#include "../fh_video_provider.c"
#include <assert.h>
static uint64_t monotonic_test_us=20000000;
static unsigned keyframe_requests;
void camera_request_keyframe(void){keyframe_requests++;}
int test_clock_gettime(clockid_t id,struct timespec *t){uint64_t us=id==CLOCK_REALTIME?UINT64_C(1700000000000000):monotonic_test_us;t->tv_sec=(time_t)(us/1000000);t->tv_nsec=(long)(us%1000000)*1000;return 0;}
int fh_frame_read(struct fh_frame_reader *r,struct fh_frame *out){(void)r;memset(out,0,sizeof *out);return FH_FRAME_EMPTY;}
static int clock_convert(void *unused,uint64_t raw,uint64_t *ns){(void)unused;*ns=raw;return 0;}
static uint32_t be32(const uint8_t *p){return(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static struct fh_frame make_frame(uint8_t *bytes,size_t n){
 memset(bytes,0xaa,n);
 const uint8_t head[]={0,0,0,1,0x67,0x42,0,0x1e,0,0,0,1,0x68,0x80,0,0,0,1,0x65};memcpy(bytes,head,sizeof head);
 return(struct fh_frame){.data=bytes,.bytes=n,.has_sps=1,.has_pps=1,.has_idr=1,.has_vcl=1,.timestamp_ns_valid=1,.sps={4,4},.pps={12,2},.timestamp_ns=UINT64_C(2000000000)};
}
static void packetize(struct camera_media_provider *provider,const struct camera_media_frame *frame){
 struct server s={.provider=provider};struct client c={.playing=1,.origin=1000000};c.video.setup=1;c.video.base=123;c.video.sequence=500;c.video.ssrc=789;c.tx=malloc(TX_CAP);assert(c.tx);
 assert(video_frame(&s,&c,frame)==0&&c.queued<=TX_CAP&&c.video.last_pts==frame->pts_us);
 size_t at=0,offset=19;unsigned packets=0,markers=0;
 while(at<c.queued){
  const uint8_t *p=c.tx+at;assert(p[0]=='$'&&p[1]==0);size_t n=((size_t)p[2]<<8)|p[3];assert(n>=13&&at+4+n<=c.queued);
  assert((((unsigned)p[6]<<8)|p[7])==(uint16_t)(500+packets));assert(be32(p+8)==90123&&be32(p+12)==789);
  assert((p[5]&127)==96);if(p[5]&128){markers++;assert(at+4+n==c.queued);}
  if(packets==0){assert(n==16&&!memcmp(p+16,frame->data+4,4));}
  else if(packets==1){assert(n==14&&!memcmp(p+16,frame->data+12,2));}
  else{
   assert((p[16]&31)==28&&(p[17]&31)==5);size_t count=n-14;
   assert(!!(p[17]&128)==(offset==19));assert(offset+count<=frame->size&&!memcmp(p+18,frame->data+offset,count));offset+=count;
   assert(!!(p[17]&64)==(offset==frame->size));
  }
  packets++;at+=n+4;
 }
 assert(offset==frame->size&&markers==1&&packets==c.video.packets);free(c.tx);
}
static unsigned callback_calls;
static struct camera_media_provider *active_provider;
static size_t server_size_override;
static int server_video(void *unused,uint64_t now,struct camera_media_frame *out){
 (void)unused;
 if(callback_calls++){stop_server=1;return 0;}
 int rc=active_provider->video(active_provider->opaque,now,out);
 if(rc==1&&server_size_override)out->size=server_size_override;
 return rc;
}
static unsigned free_port(void){
 int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(fd,(struct sockaddr *)&a,sizeof a));socklen_t n=sizeof a;assert(!getsockname(fd,(struct sockaddr *)&a,&n));unsigned port=ntohs(a.sin_port);close(fd);return port;
}
static void server_bound(struct camera_media_provider *provider,struct fh_video_provider *adapter,size_t override,int expected){
 adapter->first_pending=1;adapter->delivered=0;callback_calls=0;server_size_override=override;active_provider=provider;
 struct camera_media_provider wrapped=*provider;wrapped.video=server_video;
 struct camera_rtsp_config cfg={"127.0.0.1",free_port(),"/main",NULL,1,1000};
 assert(camera_rtsp_run(&cfg,&wrapped)==expected);assert(callback_calls==(expected?1u:2u));
}
static void pressure_recovery(void)
{
 struct camera_media_provider provider={0};provider.description.audio_rate=8000;provider.description.audio_codec=CAMERA_AUDIO_PCMA;
 struct server s={.provider=&provider};struct client c={.playing=1,.need_idr=1};
 c.video.setup=c.audio.setup=1;c.audio.channel=2;c.video.base=123;c.audio.base=456;
 c.tx=malloc(TX_CAP);uint8_t *data=malloc(140000);assert(c.tx&&data);memset(data,0xaa,140000);
 memcpy(data,(uint8_t[]){0,0,0,1,0x65},5);
 struct camera_media_frame frame={.data=data,.size=140000,.pts_us=19000000,.keyframe=1};
 assert(!video_frame(&s,&c,&frame)&&c.clock_started&&!c.need_idr);
 uint64_t origin=c.origin,sr=c.video.last_sr;int64_t wall=c.realtime_offset;
 data[4]=0x41;frame.keyframe=0;
 for(unsigned i=0;i<2;i++){frame.pts_us+=66666;assert(!video_frame(&s,&c,&frame)&&!c.need_idr);}
 size_t queued=c.queued;uint8_t *saved=malloc(queued);assert(saved);memcpy(saved,c.tx,queued);
 uint32_t packets=c.video.packets;uint16_t sequence=c.video.sequence;uint64_t pts=c.video.last_pts;
 unsigned requests=keyframe_requests;frame.pts_us+=66666;
 assert(!video_frame(&s,&c,&frame)&&c.need_idr&&keyframe_requests==requests+1);
 assert(c.queued==queued&&!memcmp(saved,c.tx,queued)&&c.video.packets==packets&&c.video.sequence==sequence&&c.video.last_pts==pts);
 assert(!video_frame(&s,&c,&frame)&&keyframe_requests==requests+1);
 struct client other={0};request_keyframe(&s,&other,monotonic_test_us);
 assert(keyframe_requests==requests+1); /* One encoder, even with many peers. */
 uint8_t audio[256]={0};struct camera_media_frame sound={.data=audio,.size=sizeof audio,.pts_us=frame.pts_us};
 assert(!audio_frame(&s,&c,&sound)&&c.queued==queued);
 monotonic_test_us+=1000000;assert(!video_frame(&s,&c,&frame)&&keyframe_requests==requests+2);
 request_keyframe(&s,&other,monotonic_test_us);assert(keyframe_requests==requests+2);
 /* A complete queued prefix drains; dependent P frames still cannot resume. */
 c.sent=c.queued;frame.pts_us+=66666;assert(!video_frame(&s,&c,&frame)&&c.need_idr&&c.sent==c.queued);
 frame.pts_us+=66666;frame.keyframe=1;data[4]=0x65;
 assert(!video_frame(&s,&c,&frame)&&!c.need_idr&&c.origin==origin&&c.realtime_offset==wall&&c.video.last_sr==sr);
 assert(c.video.last_stamp==123+(uint32_t)((frame.pts_us-origin)*9u/100u)&&!c.sent);
 assert(!audio_frame(&s,&c,&sound)&&c.audio.last_stamp==456+(uint32_t)((sound.pts_us-origin)*8000/1000000));
 /* A later malformed NAL cannot leave even a valid first NAL queued. */
 uint8_t bad[]={0,0,1,0x65,0xaa,0,0,1,0xff,0xaa};frame.data=bad;frame.size=sizeof bad;
 queued=c.queued;packets=c.video.packets;assert(video_frame(&s,&c,&frame)==-1&&c.queued==queued&&c.video.packets==packets);
 /* Saturated audio/RTCP queues hold complete packets, without client loss. */
 c.queued=TX_CAP-10;c.sent=0;packets=c.audio.packets;assert(!audio_frame(&s,&c,&sound)&&c.queued==TX_CAP-10&&c.audio.packets==packets);
 monotonic_test_us+=1000000;assert(!sender_report(&s,&c,&c.video,monotonic_test_us,0)&&c.video.last_sr==sr&&c.queued==TX_CAP-10);
 free(saved);free(data);free(c.tx);
}
int main(void){
 pressure_recovery();
 assert(CAMERA_RTSP_MAX_AU==512u*1024u);uint8_t *bytes=malloc(CAMERA_RTSP_MAX_AU+1);assert(bytes);
 struct fh_capture capture={0};struct fh_frame_reader reader={.capture=&capture,.buffer=bytes,.capacity=CAMERA_RTSP_MAX_AU,.clock={.raw_to_ns=clock_convert}};
 for(unsigned i=0;i<2;i++){
  size_t n=i?CAMERA_RTSP_MAX_AU:256u*1024u+1;
  struct fh_frame frame=make_frame(bytes,n);struct fh_video_provider adapter;struct camera_media_provider provider;
  assert(!fh_video_provider_init(&adapter,&reader,&frame,&provider));struct camera_media_frame got;
  assert(provider.video(provider.opaque,0,&got)==1&&got.size==n&&got.pts_us==2000000&&got.data==bytes);packetize(&provider,&got);
  server_bound(&provider,&adapter,0,0);
  server_bound(&provider,&adapter,CAMERA_RTSP_MAX_AU+1,-1);
  adapter.first_pending=1;adapter.delivered=0;reader.queue_owned=1;
  assert(provider.video(provider.opaque,0,&got)==-1&&adapter.last_error==-ENOTRECOVERABLE&&reader.queue_owned==1);reader.queue_owned=0;
 }
 struct fh_frame oversized=make_frame(bytes,CAMERA_RTSP_MAX_AU+1);reader.capacity=CAMERA_RTSP_MAX_AU+1;
 struct fh_video_provider adapter;struct camera_media_provider provider;
 assert(fh_video_provider_init(&adapter,&reader,&oversized,&provider)==-EMSGSIZE);
 struct fh_frame frame=make_frame(bytes,CAMERA_RTSP_MAX_AU);frame.timestamp_ns_valid=0;
 assert(fh_video_provider_init(&adapter,&reader,&frame,&provider)==-ENODATA);
 frame.timestamp_ns_valid=1;assert(!fh_video_provider_init(&adapter,&reader,&frame,&provider));bytes[5]^=1;
 struct camera_media_frame got;assert(provider.video(provider.opaque,0,&got)==-1&&adapter.last_error==-EPROTO);
 frame=make_frame(bytes,CAMERA_RTSP_MAX_AU);assert(!fh_video_provider_init(&adapter,&reader,&frame,&provider));
 assert(provider.video(provider.opaque,0,&got)==1);adapter.first_pending=1;
 assert(provider.video(provider.opaque,0,&got)==-1&&adapter.last_error==-ERANGE);
 assert(!fh_video_provider_init(&adapter,&reader,&frame,&provider));adapter.first.bytes=CAMERA_RTSP_MAX_AU+1;
 assert(provider.video(provider.opaque,0,&got)==-1&&adapter.last_error==-EMSGSIZE);
 free(bytes);puts("Native provider/server AU bounds:256KiB+1 and512KiB packetize intact;512KiB+1 rejected; ownership/PTS/parameter guards; queued-AU shedding, atomic validation, IDR/audio-clock recovery PASS");return 0;
}
