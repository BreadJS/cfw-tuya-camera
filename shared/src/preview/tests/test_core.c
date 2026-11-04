#include "../preview.c"
#include <assert.h>
#include <sys/time.h>
static uint32_t word(const uint8_t *p){return(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static const uint8_t camera_sps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xde,0x40,0xfa,0x01,0x77,0x7f,0xff,0x02,0x80};
static const uint8_t hrd_sps[]={0x27,0x4d,0,0x32,0xe7,0x40,0x3c,0x01,0x13,0xf2,0xe0,0x29,0x41,0x41,0x41,0x7c,0,0,3,0,4,0,0,3,0,0x7a,0xda,0x40,0x2e,0xe0,0x17,0x77,0xff,0xf0,0x28};
static struct preview *hub(void)
{
    static const uint8_t pps[]={0x68,0x80};
    struct preview_config config={1920,1080,camera_sps,pps,sizeof camera_sps,sizeof pps};return preview_create(&config);
}
static void changed_parameters(int hrd)
{
    const uint8_t pps_update[]={0,0,0,1,0x68,0xc0,0,0,1,0x41,0x80};
    uint8_t hrd_update[4+sizeof hrd_sps+sizeof pps_update];
    memcpy(hrd_update,"\0\0\0\1",4);memcpy(hrd_update+4,hrd_sps,sizeof hrd_sps);
    memcpy(hrd_update+4+sizeof hrd_sps,pps_update,sizeof pps_update);
    const uint8_t *update=hrd?hrd_update:pps_update;
    size_t update_size=hrd==1?4+sizeof hrd_sps:hrd?sizeof hrd_update:sizeof pps_update;
    struct preview *p=hub();assert(p);uint8_t old[2048];size_t n=p->init_size;memcpy(old,p->init,n);
    /* Parameter changes must refresh future clients even with no viewers. */
    assert(preview_publish(p,update,update_size,1000000,0)>=0);
    assert(p->config_generation==1&&p->pps[1]==(hrd==1?0x80:0xc0)&&memcmp(old,p->init,n));
    if(hrd)assert(p->sps_size==sizeof hrd_sps&&!memcmp(p->sps,hrd_sps,sizeof hrd_sps));
    preview_destroy(p);p=hub();assert(p);int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    struct timeval timeout={1,0};assert(!setsockopt(pair[1],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout));
    assert(!preview_stream(p,pair[0]));
    /* Drain exactly the old initialization before updating a waiting viewer. */
    uint8_t bytes[4096];size_t total=0;int header=0;
    while(!header||total<(size_t)header+p->init_size){
        ssize_t got=recv(pair[1],bytes+total,sizeof bytes-total,0);assert(got>0);total+=(size_t)got;
        for(size_t i=0;i+4<=total;i++)if(!memcmp(bytes+i,"\r\n\r\n",4)){header=(int)i+4;break;}
    }
    for(unsigned attempt=0;attempt<100;attempt++){
        assert(preview_publish(p,update,update_size,1000000,0)>=0);
        pthread_mutex_lock(&p->mutex);int changed=p->config_generation!=0;pthread_mutex_unlock(&p->mutex);
        if(changed)break;
        usleep(1000);
    }
    /* Old avcC cannot silently attach to the new PPS while waiting for IDR. */
    assert(recv(pair[1],bytes,sizeof bytes,0)==0);
    close(pair[1]);preview_destroy(p);
}
int main(void)
{
    static const uint8_t idr[]={0,0,0,1,0x65,0x80},pred[]={0,0,1,0x41,0x80};
    struct preview *p=hub();assert(p);p->clients=1;
    assert(!preview_publish(p,idr,sizeof idr,1000000,1));
    assert(!preview_publish(p,pred,sizeof pred,1033333,0));
    assert(p->sequence==1&&word(p->ring[0].data+88)==2999);
    assert(!preview_publish(p,pred,sizeof pred,1100000,0));
    assert(p->sequence==2&&word(p->ring[1].data+88)==6001&&word(p->ring[1].data+64)==2999);
    uint64_t generation=p->generation;
    assert(preview_publish(p,pred,sizeof pred,1099999,0)==-ERANGE);
    assert(p->waiting_key&&p->generation>generation);
    assert(preview_publish(p,pred,sizeof pred,1166666,0)==1);
    assert(!preview_publish(p,idr,sizeof idr,1200000,1));
    pthread_mutex_lock(&p->mutex);uint64_t start=now_us();
    assert(preview_publish(p,pred,sizeof pred,1266666,0)==1);assert(now_us()-start<20000);pthread_mutex_unlock(&p->mutex);
    generation=p->generation;assert(!preview_publish(p,idr,sizeof idr,1300000,1));assert(p->generation>generation);
    uint8_t changed[]={0,0,0,1,0x67,77,1,40,0,0,1,0x65,0x80};
    assert(preview_publish(p,changed,sizeof changed,1366666,1)==-ESTALE);
    p->clients=0;preview_destroy(p);
    p=hub();assert(p);int first[2],second[2],extra[2];
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,first));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,second));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,extra));
    assert(!preview_stream(p,first[0]));assert(!preview_stream(p,second[0]));assert(preview_stream(p,extra[0])==-EBUSY);
    close(extra[0]);close(extra[1]);preview_destroy(p);close(first[1]);close(second[1]);
    uint8_t tiny[1],normal[1024];size_t used;
    assert(preview_mp4_fragment(idr,sizeof idr,1,0,0,1,normal,sizeof normal,&used)==-EINVAL);
    assert(preview_mp4_fragment(idr,sizeof idr,0,0,3000,1,normal,sizeof normal,&used)==-EBADMSG);
    assert(preview_mp4_fragment(idr,sizeof idr,1,0,3000,1,tiny,sizeof tiny,&used)==-ENOSPC);
    changed_parameters(0);changed_parameters(1);changed_parameters(2);
    puts("PASS preview core: variable source PTS, positive durations, contention nonblocking, reference reset, changed SPS, max2 clients, bounded mux");return 0;
}
