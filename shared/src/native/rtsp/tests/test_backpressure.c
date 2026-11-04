/* Drive the production packetizer/queue with valid encoded video and a bounded
 * per-frame transport budget. No devices, network services or real clocks. */
#define clock_gettime test_clock_gettime
#include "../camera_rtsp.c"
#define main fixture_main
#include "fixture_server.c"
#undef main
#include <assert.h>
static uint64_t now_us=20000000;
static unsigned requests;
void camera_request_keyframe(void){requests++;}
int test_clock_gettime(clockid_t id,struct timespec *t){uint64_t us=now_us+(id==CLOCK_REALTIME?UINT64_C(1700000000000000):0);t->tv_sec=(time_t)(us/1000000);t->tv_nsec=(long)(us%1000000)*1000;return 0;}
int main(int argc,char **argv)
{
 assert(argc==3);struct fixture f={0};struct camera_media_provider provider={0};struct server s={.provider=&provider};
 f.video=read_file(argv[1],&f.video_size);assert(f.video&&!index_video(&f,&provider.description));
 struct client c={.playing=1,.need_idr=1};c.video.setup=1;c.video.base=123;c.tx=malloc(TX_CAP);assert(c.tx);
 FILE *out=fopen(argv[2],"wb");assert(out);unsigned drops=0,resyncs=0,emitted=0;
 uint64_t origin=0;int64_t wall=0;
 for(unsigned i=0;i<600;i++){
  size_t j=i%f.count;struct camera_media_frame frame={f.video+f.offset[j],f.length[j],now_us-1000,f.key[j],0,0};
  unsigned before=c.video.packets;int waiting=c.need_idr;
  assert(!video_frame(&s,&c,&frame));
  if(c.video.packets!=before){emitted++;if(waiting)resyncs++;}
  else drops++;
  if(c.clock_started){if(!origin){origin=c.origin;wall=c.realtime_offset;}assert(c.origin==origin&&c.realtime_offset==wall);}
  /* 90KB/s transport: producer's test source substantially exceeds this. */
  size_t n=c.queued-c.sent;if(n>6000)n=6000;
  assert(fwrite(c.tx+c.sent,1,n,out)==n);c.sent+=n;
  now_us+=66667;
 }
 size_t n=c.queued-c.sent;assert(fwrite(c.tx+c.sent,1,n,out)==n&&!fclose(out));
 assert(drops>30&&resyncs>2&&requests>2&&emitted>10);
 printf("Backpressure codec fixture: whole_AUs=%u skipped=%u resync_IDRs=%u keyframe_requests=%u queue_limit=%u\n",emitted,drops,resyncs,requests,TX_CAP);
 free(c.tx);free(f.video);return 0;
}
