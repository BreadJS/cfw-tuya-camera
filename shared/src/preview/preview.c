#define _GNU_SOURCE
extern void camera_request_keyframe(void) __attribute__((weak));
#include "preview.h"
#include "mp4.h"
#include "../native/rtsp/h264_parameters.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define RING_SIZE 4u
#define FRAGMENT_CAP (PREVIEW_MAX_AU+1024u)
struct slot {uint8_t data[FRAGMENT_CAP];size_t size;uint64_t sequence,generation;unsigned references;int key;};
struct client {struct preview *hub;int fd,active;};
struct preview {
    pthread_mutex_t mutex;pthread_cond_t changed;
    atomic_uint resync;
    unsigned clients,width,height;int stopping,waiting_key,pending_valid,epoch_valid;
    struct slot ring[RING_SIZE];struct client client[PREVIEW_MAX_CLIENTS];
    uint8_t pending[PREVIEW_MAX_AU],sps[256],pps[256],init[2048];
    size_t pending_size,sps_size,pps_size,init_size;
    int pending_key;
    uint64_t pending_pts,epoch,sequence,generation,config_generation;
    char mime[64];
};
static uint64_t now_us(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000+(unsigned)t.tv_nsec/1000;}
static void reset_chain(struct preview *p)
{p->pending_valid=0;p->waiting_key=1;p->generation++;pthread_cond_broadcast(&p->changed);}
static size_t prefix(const uint8_t *data,size_t bytes,size_t from,size_t *length)
{
    for(size_t i=from;i+3<=bytes;i++) {
        if(data[i]||data[i+1])continue;
        if(data[i+2]==1){*length=3;return i;}
        if(i+4<=bytes&&!data[i+2]&&data[i+3]==1){*length=4;return i;}
    }
    return bytes;
}
static int parameters_update(struct preview *p,const uint8_t *data,size_t bytes)
{
    size_t length=0,at=prefix(data,bytes,0,&length),pps_size=0,sps_size=0;
    const uint8_t *pps=NULL,*sps=NULL;
    if(at==bytes)return -EBADMSG;
    while(at<bytes) {
        size_t start=at+length,next_length=0,next=prefix(data,bytes,start+1,&next_length);
        if(start>=bytes)return -EBADMSG;
        unsigned kind=data[start]&31;
        if(kind==7){
            if(camera_h264_sps_compatible(p->sps,p->sps_size,data+start,next-start))return -ESTALE;
            sps=data+start;sps_size=next-start;
        }
        if(kind==8){
            if((data[start]&0x80)||next-start>sizeof p->pps)return -EBADMSG;
            pps=data+start;pps_size=next-start;
        }
        at=next;length=next_length;
    }
    if((sps&&(sps_size!=p->sps_size||memcmp(sps,p->sps,sps_size)))||
       (pps&&(pps_size!=p->pps_size||memcmp(pps,p->pps,pps_size)))) {
        if(!sps){sps=p->sps;sps_size=p->sps_size;}
        if(!pps){pps=p->pps;pps_size=p->pps_size;}
        uint8_t init[sizeof p->init];size_t init_size=0;
        struct preview_config config={p->width,p->height,sps,pps,sps_size,pps_size};
        int rc=preview_mp4_init(&config,init,sizeof init,&init_size);if(rc)return rc;
        if(sps!=p->sps)memcpy(p->sps,sps,sps_size);
        p->sps_size=sps_size;
        if(pps!=p->pps)memcpy(p->pps,pps,pps_size);
        p->pps_size=pps_size;
        memcpy(p->init,init,init_size);p->init_size=init_size;
        p->config_generation++;p->epoch_valid=0;reset_chain(p);
    }
    return 0;
}

struct preview *preview_create(const struct preview_config *config)
{
    struct preview *p=calloc(1,sizeof *p);if(!p)return NULL;
    if(preview_mp4_init(config,p->init,sizeof p->init,&p->init_size)){free(p);errno=EINVAL;return NULL;}
    p->width=config->width;p->height=config->height;
    memcpy(p->sps,config->sps,config->sps_size);p->sps_size=config->sps_size;
    memcpy(p->pps,config->pps,config->pps_size);p->pps_size=config->pps_size;
    snprintf(p->mime,sizeof p->mime,"video/mp4; codecs=\"avc1.%02X%02X%02X\"",p->sps[1],p->sps[2],p->sps[3]);
    int rc=pthread_mutex_init(&p->mutex,NULL);if(rc){free(p);errno=rc;return NULL;}
    pthread_condattr_t attr;rc=pthread_condattr_init(&attr);
    if(rc){pthread_mutex_destroy(&p->mutex);free(p);errno=rc;return NULL;}
    rc=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if(!rc)rc=pthread_cond_init(&p->changed,&attr);
    pthread_condattr_destroy(&attr);
    if(rc){pthread_mutex_destroy(&p->mutex);free(p);errno=rc;return NULL;}
    atomic_init(&p->resync,0);p->waiting_key=1;
    return p;
}
const char *preview_mime(const struct preview *p){return p?p->mime:NULL;}
int preview_publish(struct preview *p,const uint8_t *data,size_t size,uint64_t pts,int key)
{
    if(!p)return -EINVAL;
    if(!data||!size||size>PREVIEW_MAX_AU){atomic_store(&p->resync,1);return -EINVAL;}
    int rc=pthread_mutex_trylock(&p->mutex);
    if(rc){atomic_store(&p->resync,1);return 1;}
    if(p->stopping){pthread_mutex_unlock(&p->mutex);return -ESHUTDOWN;}
    if(atomic_exchange(&p->resync,0))reset_chain(p);
    rc=parameters_update(p,data,size);
    if(rc){reset_chain(p);pthread_mutex_unlock(&p->mutex);return rc;}
    if(!p->clients){p->pending_valid=0;p->waiting_key=1;pthread_mutex_unlock(&p->mutex);return 0;}
    if(p->waiting_key&&!key){pthread_mutex_unlock(&p->mutex);return 1;}
    if(p->epoch_valid&&pts<p->epoch){reset_chain(p);pthread_mutex_unlock(&p->mutex);return -ERANGE;}
    if(!p->epoch_valid){p->epoch=pts;p->epoch_valid=1;}
    if(p->pending_valid) {
        if(pts<=p->pending_pts||pts-p->pending_pts>UINT64_C(10000000)||pts-p->epoch>UINT64_MAX/9) {
            reset_chain(p);pthread_mutex_unlock(&p->mutex);return -ERANGE;
        }
        uint64_t previous=(p->pending_pts-p->epoch)*9/100,current=(pts-p->epoch)*9/100;
        if(current<=previous||current-previous>UINT32_MAX){reset_chain(p);pthread_mutex_unlock(&p->mutex);return -ERANGE;}
        struct slot *target=NULL;
        for(unsigned i=0;i<RING_SIZE;i++)if(!p->ring[i].references&&(!target||p->ring[i].sequence<target->sequence))target=&p->ring[i];
        if(!target){reset_chain(p);pthread_mutex_unlock(&p->mutex);return 1;}
        rc=preview_mp4_fragment(p->pending,p->pending_size,p->pending_key,previous,(uint32_t)(current-previous),
            (uint32_t)(p->sequence+1),target->data,sizeof target->data,&target->size);
        if(rc){reset_chain(p);pthread_mutex_unlock(&p->mutex);return rc;}
        target->sequence=++p->sequence;target->generation=p->generation;target->key=p->pending_key;
        pthread_cond_broadcast(&p->changed);
    }
    memcpy(p->pending,data,size);p->pending_size=size;p->pending_pts=pts;p->pending_key=!!key;p->pending_valid=1;p->waiting_key=0;
    pthread_mutex_unlock(&p->mutex);return 0;
}
static int send_bytes(int fd,const uint8_t *data,size_t bytes)
{
    uint64_t start=now_us(),progress=start;
    while(bytes) {
        uint64_t now=now_us();if(now-start>=UINT64_C(3000000)||now-progress>=UINT64_C(1000000))return -ETIMEDOUT;
        struct pollfd pollfd={fd,POLLOUT,0};int rc=poll(&pollfd,1,100);
        if(rc<0&&errno==EINTR)continue;
        if(rc<0||pollfd.revents&(POLLERR|POLLHUP|POLLNVAL))return -EPIPE;
        if(!rc)continue;
        ssize_t n=send(fd,data,bytes,MSG_NOSIGNAL);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
        if(n<=0)return -EPIPE;
        data+=n;bytes-=(size_t)n;progress=now_us();
    }
    return 0;
}
static void *stream_worker(void *opaque)
{
    struct client *c=opaque;struct preview *p=c->hub;
    static const char header[]="HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    /* Snapshot initialization atomically. A PPS update must not mutate bytes
     * being sent outside the publisher lock or let a waiting old-init viewer
     * join a later configuration generation. */
    uint8_t init[sizeof p->init];
    pthread_mutex_lock(&p->mutex);
    size_t init_size=p->init_size;memcpy(init,p->init,init_size);
    uint64_t config_generation=p->config_generation;
    uint64_t wanted=p->sequence+1,generation=p->generation;
    pthread_mutex_unlock(&p->mutex);
    int rc=send_bytes(c->fd,(const uint8_t *)header,sizeof header-1);
    if(!rc)rc=send_bytes(c->fd,init,init_size);
    pthread_mutex_lock(&p->mutex);
    uint64_t last_frame=now_us();int need_key=1;
    while(!rc&&!p->stopping) {
        if(config_generation!=p->config_generation)break;
        if(generation!=p->generation) {if(!need_key)break;generation=p->generation;wanted=p->sequence+1;}
        struct slot *slot=NULL;uint64_t earliest=UINT64_MAX;
        for(unsigned i=0;i<RING_SIZE;i++) {
            struct slot *candidate=&p->ring[i];
            if(!candidate->size||candidate->generation!=generation||candidate->sequence<wanted)continue;
            if(candidate->sequence<earliest)earliest=candidate->sequence;
            if(need_key) {if(candidate->key&&(!slot||candidate->sequence<slot->sequence))slot=candidate;}
            else if(candidate->sequence==wanted)slot=candidate;
        }
        if(!slot) {
            if((!need_key&&earliest!=UINT64_MAX&&earliest>wanted)||now_us()-last_frame>UINT64_C(5000000))break;
            struct timespec until;clock_gettime(CLOCK_MONOTONIC,&until);until.tv_nsec+=100000000;
            if(until.tv_nsec>=1000000000){until.tv_nsec-=1000000000;until.tv_sec++;}
            pthread_cond_timedwait(&p->changed,&p->mutex,&until);continue;
        }
        slot->references++;wanted=slot->sequence+1;need_key=0;
        pthread_mutex_unlock(&p->mutex);
        rc=send_bytes(c->fd,slot->data,slot->size);
        pthread_mutex_lock(&p->mutex);slot->references--;last_frame=now_us();
    }
    int fd=c->fd;c->fd=-1;c->active=0;p->clients--;pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->mutex);shutdown(fd,SHUT_RDWR);close(fd);return NULL;
}
int preview_stream(struct preview *p,int fd)
{
    if(!p||fd<0)return -EINVAL;
    int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK))return -errno;
    int buffer=32768;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof buffer);
    pthread_attr_t attr;int rc=pthread_attr_init(&attr);if(rc)return -rc;
    rc=pthread_attr_setstacksize(&attr,65536);
    if(!rc)rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(rc){pthread_attr_destroy(&attr);return -rc;}
    pthread_mutex_lock(&p->mutex);
    if(p->stopping||p->clients==PREVIEW_MAX_CLIENTS){pthread_mutex_unlock(&p->mutex);pthread_attr_destroy(&attr);return -EBUSY;}
    struct client *c=NULL;
    for(unsigned i=0;i<PREVIEW_MAX_CLIENTS;i++)if(!p->client[i].active){c=&p->client[i];break;}
    c->hub=p;c->fd=fd;c->active=1;p->clients++;
    pthread_mutex_unlock(&p->mutex);
    /* Thread creation/socket configuration must never lengthen the media
     * publisher's mutex-contention window. */
    pthread_t thread;rc=pthread_create(&thread,&attr,stream_worker,c);pthread_attr_destroy(&attr);
    if(rc){pthread_mutex_lock(&p->mutex);c->fd=-1;c->active=0;p->clients--;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);}
    if(!rc&&camera_request_keyframe)camera_request_keyframe();
    return rc?-rc:0;
}
void preview_destroy(struct preview *p)
{
    if(!p)return;
    pthread_mutex_lock(&p->mutex);p->stopping=1;
    for(unsigned i=0;i<PREVIEW_MAX_CLIENTS;i++)if(p->client[i].active)shutdown(p->client[i].fd,SHUT_RDWR);
    pthread_cond_broadcast(&p->changed);
    while(p->clients)pthread_cond_wait(&p->changed,&p->mutex);
    pthread_mutex_unlock(&p->mutex);pthread_cond_destroy(&p->changed);pthread_mutex_destroy(&p->mutex);free(p);
}
