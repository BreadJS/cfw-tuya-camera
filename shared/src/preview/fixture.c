#define _GNU_SOURCE
#include "preview.h"
#include "mp4.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define FRAMES 4096u
struct clip {uint8_t *data;size_t bytes,offset[FRAMES],size[FRAMES],count;int key[FRAMES];struct preview_config config;};
static volatile sig_atomic_t stopped;
static void stop(int signal_number){(void)signal_number;stopped=1;}
static uint64_t now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000+(unsigned)t.tv_nsec/1000;}
static size_t prefix(const uint8_t *p,size_t n,size_t from,size_t *length)
{for(size_t i=from;i+3<=n;i++){if(p[i]||p[i+1])continue;if(p[i+2]==1){*length=3;return i;}if(i+4<=n&&!p[i+2]&&p[i+3]==1){*length=4;return i;}}return n;}
static int load(struct clip *clip,const char *path)
{
    FILE *f=fopen(path,"rb");if(!f)return -1;
    if(fseek(f,0,SEEK_END)||ftell(f)<=0||ftell(f)>16*1024*1024){fclose(f);return -1;}
    clip->bytes=(size_t)ftell(f);rewind(f);clip->data=malloc(clip->bytes);
    if(!clip->data||fread(clip->data,1,clip->bytes,f)!=clip->bytes){fclose(f);return -1;}fclose(f);
    size_t length=0,at=prefix(clip->data,clip->bytes,0,&length),begin=0;int vcl=0,key=0;unsigned aud=0;
    while(at<clip->bytes) {
        size_t payload=at+length,next_length=0,next=prefix(clip->data,clip->bytes,payload+1,&next_length);
        if(payload>=clip->bytes)return -1;
        unsigned type=clip->data[payload]&31;
        if(type==9) {
            aud++;
            if(vcl){if(clip->count==FRAMES)return -1;clip->offset[clip->count]=begin;clip->size[clip->count]=at-begin;clip->key[clip->count++]=key;begin=at;vcl=key=0;}
        }
        if(type==1||type==5)vcl=1;
        if(type==5)key=1;
        if(type==7&&!clip->config.sps){clip->config.sps=clip->data+payload;clip->config.sps_size=next-payload;}
        if(type==8&&!clip->config.pps){clip->config.pps=clip->data+payload;clip->config.pps_size=next-payload;}
        at=next;length=next_length;
    }
    if(vcl&&clip->count<FRAMES){clip->offset[clip->count]=begin;clip->size[clip->count]=clip->bytes-begin;clip->key[clip->count++]=key;}
    return clip->count&&aud>=clip->count?0:-1;
}
static int dump(const struct clip *clip,const char *path,int variable)
{
    uint8_t *out=malloc(PREVIEW_MAX_AU+1024);if(!out)return -1;size_t size=0;
    FILE *f=fopen(path,"wb");if(!f){free(out);return -1;}
    int rc=preview_mp4_init(&clip->config,out,PREVIEW_MAX_AU+1024,&size);
    if(!rc&&fwrite(out,1,size,f)!=size)rc=-1;
    uint64_t pts=0;const unsigned durations[]={4500,6000,7500};
    for(size_t i=0;!rc&&i<clip->count;i++) {
        unsigned duration=variable?durations[i%3]:6000;
        rc=preview_mp4_fragment(clip->data+clip->offset[i],clip->size[i],clip->key[i],pts,duration,(uint32_t)i+1,out,PREVIEW_MAX_AU+1024,&size);
        if(!rc&&fwrite(out,1,size,f)!=size)rc=-1;
        pts+=duration;
    }
    free(out);if(fclose(f))rc=-1;return rc;
}
/* Test page only; production UI/authentication belong to control/http. */
static const char page[]="<!doctype html><meta name=viewport content='width=device-width'><video id=v muted autoplay playsinline controls style='width:90vw;max-width:960px'></video><pre id=s></pre><script>window.previewErrors=[];window.appended=0;const v=document.getElementById('v');(async()=>{const status=await(await fetch('/api/preview/status')).json();const m=new MediaSource();v.src=URL.createObjectURL(m);await new Promise(r=>m.addEventListener('sourceopen',r,{once:true}));const b=m.addSourceBuffer(status.mime);const reader=(await fetch('/api/preview.mp4')).body.getReader();for(;;){const x=await reader.read();if(x.done)break;await new Promise((r,j)=>{b.addEventListener('updateend',r,{once:true});b.addEventListener('error',j,{once:true});b.appendBuffer(x.value)});window.appended++;if(b.buffered.length){const start=b.buffered.start(0),end=b.buffered.end(b.buffered.length-1);if(v.currentTime<start||end-v.currentTime>1)v.currentTime=Math.max(start,end-.2);v.play().catch(()=>{});document.getElementById('s').textContent=JSON.stringify({time:v.currentTime,end,frames:v.getVideoPlaybackQuality().totalVideoFrames})}}})().catch(e=>{window.previewErrors.push(String(e));document.getElementById('s').textContent=String(e)});</script>";
static void simple_reply(int fd,const char *type,const char *body)
{
    char header[300];int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",type,strlen(body));
    if(send(fd,header,(size_t)n,MSG_NOSIGNAL)>0)send(fd,body,strlen(body),MSG_NOSIGNAL);
}
int main(int argc,char **argv)
{
    if(argc<5){fputs("fixture VIDEO WIDTH HEIGHT PORT [dump.mp4 [vfr]]; synthetic files only, bind127.0.0.1\n",stderr);return 2;}
    struct clip clip={0};clip.config.width=(unsigned)atoi(argv[2]);clip.config.height=(unsigned)atoi(argv[3]);
    if(load(&clip,argv[1])){free(clip.data);return 2;}
    if(argc>5){int rc=dump(&clip,argv[5],argc>6);free(clip.data);return rc?1:0;}
    struct preview *hub=preview_create(&clip.config);if(!hub){free(clip.data);return 2;}
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(listener<0)return 2;
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)atoi(argv[4])),.sin_addr={htonl(INADDR_LOOPBACK)}};
    if(bind(listener,(struct sockaddr *)&addr,sizeof addr)||listen(listener,8))return 2;
    signal(SIGTERM,stop);signal(SIGINT,stop);uint64_t epoch=now_us(),index=0;
    fprintf(stderr,"Synthetic preview fixture http://127.0.0.1:%s/ %s\n",argv[4],preview_mime(hub));
    while(!stopped) {
        struct pollfd pollfd={listener,POLLIN,0};poll(&pollfd,1,2);
        if(pollfd.revents&POLLIN) {
            int fd=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd>=0) {
                struct pollfd incoming={fd,POLLIN,0};char request[2048]={0};
                if(poll(&incoming,1,100)>0&&recv(fd,request,sizeof request-1,0)>0) {
                    if(!strncmp(request,"GET /api/preview.mp4 ",21)) {
                        if(!preview_stream(hub,fd))fd=-1;
                        else {const char busy[]="HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";send(fd,busy,sizeof busy-1,MSG_NOSIGNAL);}
                    } else if(!strncmp(request,"GET /api/preview/status ",24)) {
                        char mime[128],body[300];const char *text=preview_mime(hub);size_t k=0;
                        for(size_t i=0;text[i]&&k+2<sizeof mime;i++){if(text[i]=='"')mime[k++]='\\';mime[k++]=text[i];}mime[k]=0;
                        snprintf(body,sizeof body,"{\"available\":true,\"mime\":\"%s\",\"published\":%llu}",mime,(unsigned long long)index);simple_reply(fd,"application/json",body);
                    } else simple_reply(fd,"text/html",page);
                }
                if(fd>=0)close(fd);
            }
        }
        uint64_t now=now_us(),pts=epoch+index*1000000/15;
        if(now>=pts) {
            size_t i=(size_t)(index%clip.count);
            preview_publish(hub,clip.data+clip.offset[i],clip.size[i],pts,clip.key[i]);index++;
        }
    }
    close(listener);preview_destroy(hub);free(clip.data);return 0;
}
