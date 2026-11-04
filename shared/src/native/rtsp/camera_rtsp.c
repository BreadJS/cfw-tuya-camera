#define _GNU_SOURCE
#include "camera_rtsp.h"
#include "h264_parameters.h"
#include "../control/credentials.h"
extern void camera_request_keyframe(void) __attribute__((weak));

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define RX_CAP 8192u
#define TX_CAP (CAMERA_RTSP_MAX_AU + 16384u)
#define RTP_PAYLOAD 1200u

struct track {
    int setup;
    unsigned channel;
    uint16_t sequence;
    uint32_t ssrc, base, packets, octets, last_stamp;
    uint64_t last_pts, last_sr;
    uint64_t sample_origin, sample_next, sample_pts;
    uint32_t sample_base, sample_stamp;
    unsigned sample_rate;
};
struct client {
    int fd, playing, need_idr, closing, clock_started;
    uint8_t rx[RX_CAP], *tx;
    size_t received, sent, queued;
    uint64_t progress, activity, origin, keyframe_after, pressure_log_after;
    int64_t realtime_offset;
    char session[33];
    struct track video, audio;
};
struct server {
    struct camera_rtsp_config cfg;
    struct camera_media_provider *provider;
    struct client clients[MAX_CLIENTS];
    char authorization[1024], sps64[348], pps64[348];
    uint8_t sps[CAMERA_RTSP_MAX_PARAMETER_SET],pps[CAMERA_RTSP_MAX_PARAMETER_SET];
    size_t sps_size,pps_size;
    unsigned auth_generation;
    uint64_t accept_after; /* Resource pressure must not spin the listener. */
    uint64_t keyframe_after; /* Slow clients share the one encoder. */
};

static volatile sig_atomic_t stop_server;
static void stop_signal(int unused) { (void)unused; stop_server = 1; }

uint64_t camera_monotonic_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (unsigned)t.tv_nsec / 1000u;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static int random_bytes(void *bytes, size_t size)
{
    uint8_t *p = bytes;
    while (size) {
        /* Early boot may not have initialized the kernel CSPRNG. Never stall
         * capture/the event loop while creating a new client's secrets. */
        ssize_t n = getrandom(p, size, GRND_NONBLOCK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {if(!n)errno=EIO;return -1;}
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int base64(char *out, size_t cap, const uint8_t *input, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!cap || size > (cap - 1) / 4 * 3 || cap < ((size + 2) / 3 * 4 + 1)) return -1;
    size_t k = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)input[i] << 16;
        if (i + 1 < size) v |= (uint32_t)input[i+1] << 8;
        if (i + 2 < size) v |= input[i+2];
        out[k++] = alphabet[v >> 18]; out[k++] = alphabet[v >> 12 & 63];
        out[k++] = i + 1 < size ? alphabet[v >> 6 & 63] : '=';
        out[k++] = i + 2 < size ? alphabet[v & 63] : '=';
    }
    out[k] = 0;
    return 0;
}
static void close_client(struct client *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->tx);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}
static int append(struct client *c, const void *data, size_t size)
{
    if (c->sent) {
        memmove(c->tx, c->tx+c->sent, c->queued-c->sent);
        c->queued -= c->sent; c->sent = 0;
    }
    if (size > TX_CAP-c->queued) return -1;
    if (!c->queued) c->progress = camera_monotonic_us();
    memcpy(c->tx+c->queued, data, size);
    c->queued += size;
    return 0;
}
static int response(struct client *c, unsigned cseq, unsigned code, const char *reason,
                    const char *extra, const char *body)
{
    char header[1536];
    size_t size = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof header,
        "RTSP/1.0 %u %s\r\nCSeq: %u\r\nServer: camera-native-rtsp/0.1\r\n%sContent-Length: %zu\r\n\r\n",
        code, reason, cseq, extra ? extra : "", size);
    if (n < 0 || (size_t)n >= sizeof header || append(c, header, (size_t)n)) return -1;
    return size ? append(c, body, size) : 0;
}
static int header_value(const char *request, const char *key, char *out, size_t cap)
{
    const char *p = strstr(request, "\r\n");
    size_t keylen = strlen(key);
    while (p && p[2]) {
        p += 2;
        const char *end = strstr(p, "\r\n");
        if (!end) return -1;
        if ((size_t)(end-p) > keylen && !strncasecmp(p, key, keylen) && p[keylen] == ':') {
            p += keylen + 1;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            size_t n = (size_t)(end-p);
            if (n >= cap) return -1;
            memcpy(out, p, n); out[n] = 0;
            return 1;
        }
        p = end;
    }
    if (cap) out[0] = 0;
    return 0;
}
static int unsigned_value(const char *text, unsigned *out)
{
    if (!*text) return -1;
    unsigned long value = 0;
    for (const char *p=text; *p; p++) {
        if (*p < '0' || *p > '9' || value > 214748364u) return -1;
        value = value*10 + (unsigned)(*p-'0');
        if (value > 2147483647u) return -1;
    }
    *out = (unsigned)value;
    return 0;
}
static int equal_secret(const char *a, const char *b)
{
    size_t alen = strlen(a), blen = strlen(b);
    if (alen != blen) return 0;
    unsigned difference = 0;
    for (size_t i=0; i<alen; i++) difference |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return difference == 0;
}
static int endpoint(struct server *s, const char *uri, int *track)
{
    const char *path = uri;
    if (!strncmp(uri, "rtsp://", 7)) {
        path = strchr(uri+7, '/');
        if (!path) return -1;
    }
    size_t len = strlen(s->cfg.path);
    if (strncmp(path, s->cfg.path, len)) return -1;
    path += len;
    *track = -1;
    if (!*path || !strcmp(path, "/")) return 0;
    if (!strcmp(path, "/trackID=0")) { *track = 0; return 0; }
    if (!strcmp(path, "/trackID=1")) { *track = 1; return 0; }
    return -1;
}
static int session_ok(struct client *c, const char *request)
{
    char value[100];
    if (header_value(request, "Session", value, sizeof value) != 1) return 0;
    char *separator = strchr(value, ';'); if (separator) *separator = 0;
    return !strcmp(value, c->session);
}
static void request_keyframe(struct server *s,struct client *c,uint64_t now)
{
    if(now<c->keyframe_after||now<s->keyframe_after)return;
    c->keyframe_after=s->keyframe_after=now+1000000u;
    if(camera_request_keyframe)camera_request_keyframe();
}
static int request(struct server *s, struct client *c, char *text)
{
    char method[32], uri[512], version[32], value[1024], extra[1200], body[2048];
    unsigned cseq, length = 0;
    int track;
    if (sscanf(text, "%31s %511s %31s", method, uri, version) != 3 || strcmp(version,"RTSP/1.0") ||
        header_value(text,"CSeq",value,sizeof value) != 1 || unsigned_value(value,&cseq)) return -1;
    int got = header_value(text,"Content-Length",value,sizeof value);
    if (got < 0 || (got && (unsigned_value(value,&length) || length))) {
        c->closing=1;
        return response(c,cseq,400,"Bad Request",NULL,NULL);
    }
    char current_auth[1024];const char *expected=s->authorization;
    if(camera_credentials_token){int rc=camera_credentials_token(1,current_auth,sizeof current_auth,NULL);if(rc<0)return -1;if(rc>0)expected=current_auth;}
    if (*expected && (header_value(text,"Authorization",value,sizeof value) != 1 ||
                             !equal_secret(value,expected)))
        return response(c,cseq,401,"Unauthorized","WWW-Authenticate: Basic realm=\"camera-local\"\r\n",NULL);
    if (!strcmp(method,"OPTIONS"))
        return response(c,cseq,200,"OK","Public: OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN\r\n",NULL);
    if (endpoint(s,uri,&track)) return response(c,cseq,404,"Not Found",NULL,NULL);
    if (!strcmp(method,"DESCRIBE")) {
        const struct camera_media_description *d=&s->provider->description;
        char audio[256]="";
        if (d->audio_codec != CAMERA_AUDIO_NONE)
            snprintf(audio,sizeof audio,"m=audio 0 RTP/AVP %u\r\na=rtpmap:%u %s/%u/1\r\na=control:trackID=1\r\n",
                     d->audio_codec==CAMERA_AUDIO_PCMA?8u:97u, d->audio_codec==CAMERA_AUDIO_PCMA?8u:97u,
                     d->audio_codec==CAMERA_AUDIO_PCMA?"PCMA":"L16",d->audio_rate);
        int n=snprintf(body,sizeof body,
            "v=0\r\no=- 1 1 IN IP4 %s\r\ns=Independent camera media\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n"
            "a=control:trackID=0\r\n%s",
            s->cfg.bind_address,s->sps[1],s->sps[2],s->sps[3],s->sps64,s->pps64,audio);
        if (n<0 || (size_t)n>=sizeof body) return -1;
        n=snprintf(extra,sizeof extra,"Content-Type: application/sdp\r\nContent-Base: %s%s\r\n",uri,
                   uri[strlen(uri)-1]=='/'?"":"/");
        if (n<0 || (size_t)n>=sizeof extra) return -1;
        return response(c,cseq,200,"OK",extra,body);
    }
    if (!strcmp(method,"SETUP")) {
        if (c->playing) return response(c,cseq,455,"Method Not Valid in This State",NULL,NULL);
        if (track<0 || (track==1 && s->provider->description.audio_codec==CAMERA_AUDIO_NONE))
            return response(c,cseq,404,"Not Found",NULL,NULL);
        if (header_value(text,"Transport",value,sizeof value)!=1 ||
            strncasecmp(value,"RTP/AVP/TCP",11))
            return response(c,cseq,461,"Unsupported Transport",NULL,NULL);
        char *channels=strstr(value,"interleaved=");
        unsigned first, second; int consumed=0;
        if (!channels || sscanf(channels+12,"%u-%u%n",&first,&second,&consumed)!=2 ||
            (channels[12+consumed] && channels[12+consumed]!=';') || first>254 || second!=first+1)
            return response(c,cseq,461,"Unsupported Transport",NULL,NULL);
        struct track *target=track?&c->audio:&c->video, *other=track?&c->video:&c->audio;
        if (target->setup || (other->setup && (first==other->channel || first==other->channel+1 || second==other->channel)))
            return response(c,cseq,455,"Method Not Valid in This State",NULL,NULL);
        target->setup=1; target->channel=first;
        snprintf(extra,sizeof extra,"Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X\r\nSession: %s;timeout=30\r\n",
                 first,second,target->ssrc,c->session);
        return response(c,cseq,200,"OK",extra,NULL);
    }
    if (!strcmp(method,"PLAY")) {
        if (!session_ok(c,text)) return response(c,cseq,454,"Session Not Found",NULL,NULL);
        if (!c->video.setup || c->playing) return response(c,cseq,455,"Method Not Valid in This State",NULL,NULL);
        c->playing=1; c->need_idr=1;
        request_keyframe(s,c,camera_monotonic_us());
        snprintf(extra,sizeof extra,"Session: %s\r\nRange: npt=0.000-\r\n",c->session);
        return response(c,cseq,200,"OK",extra,NULL);
    }
    if (!strcmp(method,"GET_PARAMETER")) {
        if (!session_ok(c,text)) return response(c,cseq,454,"Session Not Found",NULL,NULL);
        snprintf(extra,sizeof extra,"Session: %s\r\n",c->session);
        return response(c,cseq,200,"OK",extra,NULL);
    }
    if (!strcmp(method,"TEARDOWN")) {
        if (!session_ok(c,text)) return response(c,cseq,454,"Session Not Found",NULL,NULL);
        c->playing=0; c->closing=1;
        return response(c,cseq,200,"OK",NULL,NULL);
    }
    return response(c,cseq,405,"Method Not Allowed",NULL,NULL);
}
static int receive_requests(struct server *s, struct client *c)
{
    ssize_t n=recv(c->fd,c->rx+c->received,RX_CAP-c->received,0);
    if (n<0) return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR ? 0 : -1;
    if (!n) return -1;
    c->received+=(size_t)n; c->activity=camera_monotonic_us();
    while (c->received) {
        size_t consumed;
        if (c->rx[0]=='$') {
            if (c->received<4) break;
            consumed=4u+((unsigned)c->rx[2]<<8)+c->rx[3];
            if (consumed>RX_CAP) return -1;
            if (c->received<consumed) break;
        } else {
            size_t end=0;
            for (size_t i=0;i+3<c->received;i++)
                if (!memcmp(c->rx+i,"\r\n\r\n",4)) { end=i+4; break; }
            if (!end) { if (c->received==RX_CAP) return -1; break; }
            char text[RX_CAP+1]; memcpy(text,c->rx,end); text[end]=0;
            if (memchr(text,0,end) || request(s,c,text)) return -1;
            consumed=end;
        }
        c->received-=consumed; memmove(c->rx,c->rx+consumed,c->received);
    }
    return 0;
}
static int rtp(struct client *c, struct track *t, unsigned payload_type,
               uint32_t timestamp, int marker, const uint8_t *data, size_t size)
{
    uint8_t packet[4+12+RTP_PAYLOAD];
    if (size>RTP_PAYLOAD) return -1;
    packet[0]='$';packet[1]=(uint8_t)t->channel;put16(packet+2,(uint16_t)(12+size));
    packet[4]=0x80;packet[5]=(uint8_t)(payload_type|(marker?128:0));put16(packet+6,t->sequence++);
    put32(packet+8,timestamp);put32(packet+12,t->ssrc);memcpy(packet+16,data,size);
    if (append(c,packet,16+size)) return -1;
    t->packets++;t->octets+=(uint32_t)size;t->last_stamp=timestamp;
    return 0;
}
static int nal(struct client *c, struct track *t, uint32_t stamp, int last,
               const uint8_t *data,size_t size)
{
    if (!size || (data[0]&128) || !(data[0]&31) || (data[0]&31)>23) return -1;
    if (size<=RTP_PAYLOAD) return rtp(c,t,96,stamp,last,data,size);
    uint8_t fragment[RTP_PAYLOAD];
    fragment[0]=(data[0]&0xe0)|28;
    size_t position=1;
    while (position<size) {
        size_t count=size-position;
        if (count>RTP_PAYLOAD-2) count=RTP_PAYLOAD-2;
        int end=position+count==size;
        fragment[1]=(data[0]&31)|(position==1?128:0)|(end?64:0);
        memcpy(fragment+2,data+position,count);
        if (rtp(c,t,96,stamp,last&&end,fragment,count+2)) return -1;
        position+=count;
    }
    return 0;
}
static int start_code(const uint8_t *data,size_t size,size_t position,size_t *prefix)
{
    if (position+3<=size && !data[position] && !data[position+1]) {
        if (data[position+2]==1) { *prefix=3;return 1; }
        if (position+4<=size && !data[position+2] && data[position+3]==1) { *prefix=4;return 1; }
    }
    return 0;
}
static int sender_report(struct server *,struct client *,struct track *,uint64_t,int);
/* Validate and size the complete AU before modifying a client's queue or RTP
 * sequence. A failed append halfway through a large IDR used to close the
 * connection and strand a truncated frame in the peer's decoder. */
static int video_packet_bytes(const struct camera_media_frame *frame,size_t *bytes)
{
    if(!frame||!frame->data||!frame->size||frame->size>CAMERA_RTSP_MAX_AU)return -1;
    size_t position=0,total=0,prefix;
    while(position<frame->size){
        if(!start_code(frame->data,frame->size,position,&prefix))return -1;
        size_t begin=position+prefix,end=begin,next_prefix;
        while(end<frame->size&&!start_code(frame->data,frame->size,end,&next_prefix))end++;
        size_t n=end-begin;
        if(!n||(frame->data[begin]&128)||!(frame->data[begin]&31)||(frame->data[begin]&31)>23)return -1;
        size_t cost=n<=RTP_PAYLOAD?n+16:(n-1)+18*((n-1+RTP_PAYLOAD-3)/(RTP_PAYLOAD-2));
        if(cost>TX_CAP-total)return -1;
        total+=cost;position=end;
    }
    *bytes=total;return 0;
}
/* Refresh SDP from bounded in-band parameters after validating the entire AU.
 * Decoder syntax stays fixed while VUI/HRD metadata may change. */
static int video_parameters(struct server *s,const struct camera_media_frame *frame)
{
    size_t position=0,prefix,pps_size=0,sps_size=0;const uint8_t *pps=NULL,*sps=NULL;
    while(position<frame->size){
        if(!start_code(frame->data,frame->size,position,&prefix))return -1;
        size_t begin=position+prefix,end=begin,next_prefix;
        while(end<frame->size&&!start_code(frame->data,frame->size,end,&next_prefix))end++;
        if(begin==end||(frame->data[begin]&128)||!(frame->data[begin]&31)||(frame->data[begin]&31)>23)return -1;
        unsigned type=frame->data[begin]&31;
        if(type==7){
            if(camera_h264_sps_compatible(s->sps,s->sps_size,frame->data+begin,end-begin))return -1;
            sps=frame->data+begin;sps_size=end-begin;
        }
        if(type==8){if(end-begin>sizeof s->pps)return -1;pps=frame->data+begin;pps_size=end-begin;}
        position=end;
    }
    if(sps&&(sps_size!=s->sps_size||memcmp(sps,s->sps,sps_size))){
        if(base64(s->sps64,sizeof s->sps64,sps,sps_size))return -1;
        memcpy(s->sps,sps,sps_size);s->sps_size=sps_size;
    }
    if(pps&&(pps_size!=s->pps_size||memcmp(pps,s->pps,pps_size))){
        if(base64(s->pps64,sizeof s->pps64,pps,pps_size))return -1;
        memcpy(s->pps,pps,pps_size);s->pps_size=pps_size;
    }
    return 0;
}
/* One nonblocking write attempt. EINTR has transmitted no bytes and must not
 * terminate a partially sent interleaved RTP packet or its owning session. */
static int flush_client(struct client *c,uint64_t now)
{
    if(c->queued<=c->sent)return 0;
    ssize_t n=send(c->fd,c->tx+c->sent,c->queued-c->sent,MSG_NOSIGNAL);
    if(n>0){c->sent+=(size_t)n;c->progress=now;}
    else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return -1;
    if(c->sent==c->queued)c->sent=c->queued=0;
    return 0;
}
/* Keep room for one maximum audio block and both periodic clock reports. */
#define MEDIA_RESERVE (CAMERA_RTSP_MAX_AUDIO+16*((CAMERA_RTSP_MAX_AUDIO+RTP_PAYLOAD-1)/RTP_PAYLOAD)+152u)
static int video_frame(struct server *s,struct client *c,const struct camera_media_frame *frame)
{
    if (!c->playing) return 0;
    uint64_t now=camera_monotonic_us();
    if(c->need_idr&&!frame->keyframe){request_keyframe(s,c,now);return 0;}
    size_t packet_bytes;
    if(video_packet_bytes(frame,&packet_bytes))return -1;
    int sync=c->need_idr;
    size_t parameter_bytes=sync?((s->sps_size?s->sps_size+16:0)+(s->pps_size?s->pps_size+16:0)):0;
    packet_bytes+=parameter_bytes;
    int initial=c->need_idr&&!c->clock_started;
    size_t reports=initial?76u*((unsigned)c->video.setup+(unsigned)c->audio.setup):0;
    if(packet_bytes+reports+MEDIA_RESERVE>TX_CAP)return -1;
    if(packet_bytes+reports+MEDIA_RESERVE>TX_CAP-(c->queued-c->sent)){
        /* Retain every already-queued byte. Drop the WHOLE new AU and all
         * dependent pictures until a complete IDR fits. Capture keeps running
         * for other clients, with the original session clock/audio anchor. */
        c->need_idr=1;request_keyframe(s,c,now);
        if(now>=c->pressure_log_after){
            fprintf(stderr,"RTSP_BACKPRESSURE au_bytes=%zu queued_bytes=%zu waiting_idr=yes\n",frame->size,c->queued-c->sent);
            c->pressure_log_after=now+1000000u;
        }
        return 0;
    }
    if(initial) {
        now=camera_monotonic_us();
        c->origin=frame->pts_us;
        /* Anchor each session after network time may have corrected the boot
         * clock. Keep that anchor stable throughout queue-pressure recovery. */
        struct timespec real;clock_gettime(CLOCK_REALTIME,&real);
        c->realtime_offset=(int64_t)real.tv_sec*1000000+real.tv_nsec/1000-(int64_t)now;
        if(sender_report(s,c,&c->video,now,1)||sender_report(s,c,&c->audio,now,1))return -1;
        c->clock_started=1;
    }
    if(frame->pts_us<c->origin||(c->video.packets&&frame->pts_us<c->video.last_pts))return -1;
    c->need_idr=0;
    uint32_t stamp=c->video.base+(uint32_t)((frame->pts_us-c->origin)*9u/100u);
    if(sync){
        if(s->sps_size&&nal(c,&c->video,stamp,0,s->sps,s->sps_size))return -1;
        if(s->pps_size&&nal(c,&c->video,stamp,0,s->pps,s->pps_size))return -1;
    }
    size_t position=0,prefix;
    while (position<frame->size) {
        if (!start_code(frame->data,frame->size,position,&prefix)) return -1;
        size_t begin=position+prefix,end=begin, next_prefix;
        while (end<frame->size && !start_code(frame->data,frame->size,end,&next_prefix)) end++;
        if (nal(c,&c->video,stamp,end==frame->size,frame->data+begin,end-begin)) return -1;
        position=end;
    }
    c->video.last_pts=frame->pts_us;
    return 0;
}
static int audio_frame(struct server *s,struct client *c,const struct camera_media_frame *frame)
{
    if (!c->playing || c->need_idr || !c->audio.setup || frame->pts_us<c->origin) return 0;
    unsigned rate=s->provider->description.audio_rate;
    unsigned width=s->provider->description.audio_codec==CAMERA_AUDIO_L16?2u:1u;
    unsigned pt=s->provider->description.audio_codec==CAMERA_AUDIO_PCMA?8u:97u;
    if (!frame->data||!frame->size||frame->size>CAMERA_RTSP_MAX_AUDIO||frame->size%width||!rate) return -1;
    struct track *t=&c->audio;
    unsigned actual=frame->sample_clock_rate;
    size_t samples=frame->size/width;
    if((t->packets||t->sample_rate)&&frame->pts_us<t->last_pts)return -1;
    if(actual){
        if(actual<8000||actual>192000||frame->sample_position>UINT64_MAX-samples)return -1;
        if(t->sample_rate&&(actual!=t->sample_rate||frame->sample_position<t->sample_next))return -1;
    }else if(t->sample_rate)return -1;
    if(frame->pts_us-c->origin>UINT64_MAX/rate)return -1;
    /* Audio blocks have no inter-frame dependencies. Skip a whole block if
     * a slow peer has consumed the reserved headroom; never append a prefix. */
    size_t cost=frame->size+16*((frame->size+RTP_PAYLOAD-1)/RTP_PAYLOAD);
    if(cost>TX_CAP-(c->queued-c->sent))return 0;
    uint32_t stamp=c->audio.base+(uint32_t)((frame->pts_us-c->origin)*rate/1000000u);
    if(actual){
        /* Only the first block uses its common-clock PTS to join the video
         * session. Quantized SDK milliseconds must never replace exact sample
         * positions for subsequent RTP blocks, including source drop gaps. */
        if(!t->sample_rate){t->sample_origin=frame->sample_position;t->sample_base=stamp;t->sample_rate=actual;}
        stamp=t->sample_base+(uint32_t)(frame->sample_position-t->sample_origin);
    }
    for (size_t position=0;position<frame->size;) {
        size_t count=frame->size-position;
        if (count>RTP_PAYLOAD) count=RTP_PAYLOAD;
        if (rtp(c,&c->audio,pt,stamp+(uint32_t)(position/width),0,frame->data+position,count)) return -1;
        position+=count;
    }
    t->last_pts=frame->pts_us;
    if(actual){
        t->sample_next=frame->sample_position+samples;
        t->sample_pts=frame->pts_us;t->sample_stamp=stamp;
    }
    return 0;
}
static int sender_report(struct server *s,struct client *c,struct track *t,uint64_t now,int initial)
{
    if (!t->setup || (!initial&&(!t->packets||now<=t->last_sr||now-t->last_sr<1000000u))) return 0;
    /* RFC3550 6.4.1: both SR clocks describe this report's sending instant,
     * even when the most recent media packet is old. Keep source packet PTS
     * unchanged. Audio with an exact sample counter extrapolates its latest
     * sample/PTS pair at the measured source rate; SDP remains nominal. */
    unsigned rate=t==&c->video?90000u:s->provider->description.audio_rate;
    if(!rate || now<c->origin || now-c->origin>UINT64_MAX/rate)return -1;
    uint64_t wall=(uint64_t)((int64_t)now+c->realtime_offset);
    uint32_t report_stamp=t->base+(uint32_t)((now-c->origin)*rate/1000000u);
    if(t==&c->audio&&t->sample_rate){
        if(now<t->sample_pts||now-t->sample_pts>UINT64_MAX/t->sample_rate)return -1;
        report_stamp=t->sample_stamp+(uint32_t)((now-t->sample_pts)*t->sample_rate/1000000u);
    }
    /* Compound SR + SDES/CNAME, shared CNAME across the audio/video tracks. */
    uint8_t packet[76]={'$',(uint8_t)(t->channel+1),0,72,0x80,200,0,6};
    put32(packet+8,t->ssrc);
    put32(packet+12,(uint32_t)(wall/1000000u+2208988800u));
    put32(packet+16,(uint32_t)((wall%1000000u)*(UINT64_C(1)<<32)/1000000u));
    put32(packet+20,report_stamp);put32(packet+24,t->packets);put32(packet+28,t->octets);
    packet[32]=0x81;packet[33]=202;put16(packet+34,10);put32(packet+36,t->ssrc);
    packet[40]=1;packet[41]=32;
    /* Session tokens are not used as CNAMEs; derive a public stable identifier. */
    memset(packet+42,'0',32);
    snprintf((char *)packet+42,17,"camera-%08X",c->video.ssrc);
    for(unsigned i=42;i<74;i++)if(!packet[i])packet[i]='0';
    packet[74]=packet[75]=0;
    if(sizeof packet>TX_CAP-(c->queued-c->sent))return 0;
    t->last_sr=now;
    return append(c,packet,sizeof packet);
}
static int accept_client(struct server *s,int listener,uint64_t now)
{
    int fd=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if (fd<0) {
        int error=errno;
        if(error==EAGAIN || error==EWOULDBLOCK)return 0;
        /* Only a broken listening descriptor is fatal. Linux can propagate
         * pending network errors from accept; descriptor/memory pressure and
         * interruptions also do not invalidate existing client connections. */
        if(error==EBADF || error==ENOTSOCK || error==EINVAL || error==EOPNOTSUPP)return -1;
        s->accept_after=now+100000u;
        return 0;
    }
    /* Small low-latency RTP access units must not wait for Nagle batching
     * while the peer uses delayed acknowledgements. Slow-client bounds and
     * the nonblocking output queue remain in force. */
    int nodelay=1;
    if(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&nodelay,sizeof nodelay)) {close(fd);s->accept_after=now+100000u;return 0;}
    for (unsigned i=0;i<s->cfg.max_clients;i++) {
        struct client *c=&s->clients[i];
        if (c->fd>=0) continue;
        uint8_t random[32];
        if (random_bytes(random,sizeof random)) {
            close(fd);
            s->accept_after=now+100000u;
            /* Retry only through a later client connection once entropy is
             * ready. Keep existing clients and capture running, without a
             * predictable fallback session token, SSRC or sequence seed. */
            return 0;
        }
        c->tx=malloc(TX_CAP);
        if (!c->tx) {close(fd);s->accept_after=now+100000u;return 0;}
        c->fd=fd;c->activity=c->progress=now;
        for (unsigned j=0;j<16;j++) snprintf(c->session+2*j,3,"%02x",random[j]);
        memcpy(&c->video.ssrc,random+16,4);memcpy(&c->audio.ssrc,random+20,4);
        memcpy(&c->video.base,random+24,4);memcpy(&c->audio.base,random+28,4);
        c->video.sequence=(uint16_t)c->video.base;c->audio.sequence=(uint16_t)c->audio.base;
        int buffer=32768;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof buffer);
        return 0;
    }
    close(fd);
    return 0;
}
int camera_rtsp_run(const struct camera_rtsp_config *config,struct camera_media_provider *provider)
{
    struct server s={0};
    struct in_addr bind_address;
    if (!config || !provider) return -1;
    const struct camera_media_description *d=&provider->description;
    if (!provider->video || !config->path || config->path[0]!='/' ||
        strlen(config->path)>128 || !config->bind_address || !config->port || config->port>65535 ||
        inet_pton(AF_INET,config->bind_address,&bind_address)!=1 ||
        !config->max_clients || config->max_clients>MAX_CLIENTS || config->slow_timeout_ms<50 ||
        !d->sps || d->sps_size<4 || d->sps_size>CAMERA_RTSP_MAX_PARAMETER_SET ||
        !d->pps || !d->pps_size || d->pps_size>CAMERA_RTSP_MAX_PARAMETER_SET ||
        d->audio_codec<CAMERA_AUDIO_NONE || d->audio_codec>CAMERA_AUDIO_L16 ||
        (d->audio_codec!=CAMERA_AUDIO_NONE && (!provider->audio || d->audio_rate<8000 || d->audio_rate>48000)) ||
        (d->audio_codec==CAMERA_AUDIO_PCMA && d->audio_rate!=8000)) return -1;
    if (ntohl(bind_address.s_addr)!=INADDR_LOOPBACK && (!config->credentials || !*config->credentials)) {
        fputs("Non-loopback binding requires credentials and a trusted LAN/VPN\n",stderr);return -1;
    }
    s.cfg=*config;s.provider=provider;
    memcpy(s.sps,d->sps,d->sps_size);s.sps_size=d->sps_size;
    memcpy(s.pps,d->pps,d->pps_size);s.pps_size=d->pps_size;
    if (base64(s.sps64,sizeof s.sps64,d->sps,d->sps_size) ||
        base64(s.pps64,sizeof s.pps64,d->pps,d->pps_size)) return -1;
    if (config->credentials) {
        if (!strchr(config->credentials,':') || strchr(config->credentials,'\r') || strchr(config->credentials,'\n')) return -1;
        memcpy(s.authorization,"Basic ",6);
        if (base64(s.authorization+6,sizeof s.authorization-6,(const uint8_t *)config->credentials,strlen(config->credentials))) return -1;
    }
    for (unsigned i=0;i<MAX_CLIENTS;i++)s.clients[i].fd=-1;
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (listener<0)return -1;
    int reuse=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)config->port),.sin_addr=bind_address};
    if (bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,8)) {close(listener);return -1;}
    struct sigaction previous_int,previous_term,action={.sa_handler=stop_signal};
    sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,&previous_int);sigaction(SIGTERM,&action,&previous_term);
    stop_server=0;
    int result=0;
    while (!stop_server) {
        uint64_t accept_now=camera_monotonic_us();
        struct pollfd fds[MAX_CLIENTS+1]={{.fd=accept_now<s.accept_after?-1:listener,.events=POLLIN}};
        for (unsigned i=0;i<config->max_clients;i++) {
            struct client *c=&s.clients[i];fds[i+1].fd=c->fd;
            fds[i+1].events=POLLIN|(c->queued>c->sent?POLLOUT:0);
        }
        if(camera_credentials_token) {
            char token[1024];unsigned generation=0;
            int rc=camera_credentials_token(1,token,sizeof token,&generation);
            if(rc<0)break;
            if(rc>0&&s.auth_generation!=generation){
                for(unsigned i=0;i<config->max_clients;i++)close_client(&s.clients[i]);
                s.auth_generation=generation;continue;
            }
        }
        int ready=poll(fds,config->max_clients+1,5);
        if (ready<0 && errno!=EINTR) {result=-1;break;}
        uint64_t now=camera_monotonic_us();
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL)){result=-1;break;}
        if (fds[0].revents&POLLIN) if (accept_client(&s,listener,now)) {result=-1;break;}
        for (unsigned i=0;i<config->max_clients;i++) {
            struct client *c=&s.clients[i];if(c->fd<0)continue;
            short events=fds[i+1].revents;
            if ((events&(POLLERR|POLLHUP|POLLNVAL)) ||
                ((events&POLLIN)&&receive_requests(&s,c))) {close_client(c);continue;}
            if ((events&POLLOUT)&&c->queued>c->sent) {
                if(flush_client(c,now)){close_client(c);continue;}
            }
            if((c->closing&&!c->queued) || (now>c->activity&&now-c->activity>30000000u) ||
               (c->queued&&now>c->progress&&now-c->progress>(uint64_t)config->slow_timeout_ms*1000))close_client(c);
        }
        struct camera_media_frame frame;
        int got=provider->video(provider->opaque,now,&frame);
        if(got<0 || (got && (!frame.data||!frame.size||frame.size>CAMERA_RTSP_MAX_AU))){result=-1;break;}
        if(got&&video_parameters(&s,&frame)){result=-1;break;}
        if(got)for(unsigned i=0;i<config->max_clients;i++)if(s.clients[i].fd>=0&&video_frame(&s,&s.clients[i],&frame))close_client(&s.clients[i]);
        if(provider->audio) {
            got=provider->audio(provider->opaque,now,&frame);
            if(got<0 || (got&&(!frame.data||!frame.size||frame.size>CAMERA_RTSP_MAX_AUDIO))){result=-1;break;}
            if(got)for(unsigned i=0;i<config->max_clients;i++)if(s.clients[i].fd>=0&&audio_frame(&s,&s.clients[i],&frame))close_client(&s.clients[i]);
        }
        /* Provider reads and initial reports can advance beyond the poll's
         * earlier timestamp. Never regress a report clock or underflow last_sr. */
        now=camera_monotonic_us();
        for(unsigned i=0;i<config->max_clients;i++) {
            struct client *c=&s.clients[i];if(c->fd<0||!c->playing)continue;
            if(sender_report(&s,c,&c->video,now,0)||sender_report(&s,c,&c->audio,now,0))close_client(c);
        }
    }
    for(unsigned i=0;i<MAX_CLIENTS;i++)close_client(&s.clients[i]);
    close(listener);sigaction(SIGINT,&previous_int,NULL);sigaction(SIGTERM,&previous_term,NULL);
    return result;
}
