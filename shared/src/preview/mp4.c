#include "mp4.h"
#include <errno.h>
#include <string.h>
struct writer { uint8_t *p;size_t used,capacity;int failed; };
static void bytes(struct writer *w,const void *p,size_t n)
{if(n>w->capacity-w->used){w->failed=1;return;}memcpy(w->p+w->used,p,n);w->used+=n;}
static void u8(struct writer *w,unsigned n){uint8_t v=(uint8_t)n;bytes(w,&v,1);}
static void u16(struct writer *w,unsigned n){u8(w,n>>8);u8(w,n);}
static void u32(struct writer *w,uint32_t n){u16(w,n>>16);u16(w,n);}
static void u64(struct writer *w,uint64_t n){u32(w,(uint32_t)(n>>32));u32(w,(uint32_t)n);}
static void zero(struct writer *w,unsigned n){while(n--)u8(w,0);}
static size_t box(struct writer *w,const char type[4])
{size_t start=w->used;u32(w,0);bytes(w,type,4);return start;}
static void finish(struct writer *w,size_t start)
{
    if(w->failed)return;
    uint32_t n=(uint32_t)(w->used-start);
    w->p[start]=(uint8_t)(n>>24);w->p[start+1]=(uint8_t)(n>>16);
    w->p[start+2]=(uint8_t)(n>>8);w->p[start+3]=(uint8_t)n;
}
static void matrix(struct writer *w)
{const uint32_t m[]={0x10000,0,0,0,0x10000,0,0,0,0x40000000};for(unsigned i=0;i<9;i++)u32(w,m[i]);}
int preview_mp4_init(const struct preview_config *c,uint8_t *out,size_t capacity,size_t *used)
{
    if(used)*used=0;
    if(!c||!out||!used||!c->width||!c->height||c->width>4096||c->height>4096||
       !c->sps||!c->pps||c->sps_size<4||c->sps_size>256||!c->pps_size||c->pps_size>256||
       (c->sps[0]&0x9f)!=7||(c->pps[0]&0x9f)!=8||(c->sps[1]!=66&&c->sps[1]!=77))return -EINVAL;
    struct writer w={out,0,capacity,0};
    size_t b=box(&w,"ftyp");bytes(&w,"iso6",4);u32(&w,1);bytes(&w,"isomiso6avc1mp41",16);finish(&w,b);
    size_t moov=box(&w,"moov");
    b=box(&w,"mvhd");u32(&w,0);zero(&w,8);u32(&w,90000);u32(&w,0);u32(&w,0x10000);u16(&w,0x100);zero(&w,10);matrix(&w);zero(&w,24);u32(&w,2);finish(&w,b);
    size_t trak=box(&w,"trak");
    b=box(&w,"tkhd");u32(&w,7);zero(&w,8);u32(&w,1);zero(&w,20);u16(&w,0);u16(&w,0);matrix(&w);u32(&w,c->width<<16);u32(&w,c->height<<16);finish(&w,b);
    size_t mdia=box(&w,"mdia");
    b=box(&w,"mdhd");u32(&w,0);zero(&w,8);u32(&w,90000);u32(&w,0);u16(&w,0x55c4);u16(&w,0);finish(&w,b);
    b=box(&w,"hdlr");u32(&w,0);u32(&w,0);bytes(&w,"vide",4);zero(&w,12);bytes(&w,"Native preview",15);finish(&w,b);
    size_t minf=box(&w,"minf");
    b=box(&w,"vmhd");u32(&w,1);zero(&w,8);finish(&w,b);
    size_t dinf=box(&w,"dinf"),dref=box(&w,"dref");u32(&w,0);u32(&w,1);
    b=box(&w,"url ");u32(&w,1);finish(&w,b);finish(&w,dref);finish(&w,dinf);
    size_t stbl=box(&w,"stbl"),stsd=box(&w,"stsd");u32(&w,0);u32(&w,1);
    size_t avc1=box(&w,"avc1");zero(&w,6);u16(&w,1);zero(&w,16);u16(&w,c->width);u16(&w,c->height);
    u32(&w,72u<<16);u32(&w,72u<<16);u32(&w,0);u16(&w,1);zero(&w,32);u16(&w,0x18);u16(&w,0xffff);
    b=box(&w,"avcC");u8(&w,1);u8(&w,c->sps[1]);u8(&w,c->sps[2]);u8(&w,c->sps[3]);u8(&w,0xff);u8(&w,0xe1);
    u16(&w,(unsigned)c->sps_size);bytes(&w,c->sps,c->sps_size);u8(&w,1);u16(&w,(unsigned)c->pps_size);bytes(&w,c->pps,c->pps_size);finish(&w,b);
    finish(&w,avc1);finish(&w,stsd);
    b=box(&w,"stts");u32(&w,0);u32(&w,0);finish(&w,b);
    b=box(&w,"stsc");u32(&w,0);u32(&w,0);finish(&w,b);
    b=box(&w,"stsz");u32(&w,0);u32(&w,0);u32(&w,0);finish(&w,b);
    b=box(&w,"stco");u32(&w,0);u32(&w,0);finish(&w,b);
    finish(&w,stbl);finish(&w,minf);finish(&w,mdia);finish(&w,trak);
    size_t mvex=box(&w,"mvex");b=box(&w,"trex");u32(&w,0);u32(&w,1);u32(&w,1);zero(&w,12);finish(&w,b);finish(&w,mvex);finish(&w,moov);
    if(w.failed)return -ENOSPC;
    *used=w.used;return 0;
}
static size_t prefix(const uint8_t *p,size_t n,size_t from,size_t *length)
{
    for(size_t i=from;i+3<=n;i++) {
        if(p[i]||p[i+1])continue;
        if(p[i+2]==1){*length=3;return i;}
        if(i+4<=n&&!p[i+2]&&p[i+3]==1){*length=4;return i;}
    }
    return n;
}
int preview_mp4_fragment(const uint8_t *au,size_t size,int key,uint64_t decode_time,
                         uint32_t duration,uint32_t sequence,uint8_t *out,size_t capacity,size_t *used)
{
    if(used)*used=0;
    if(!au||!out||!used||!size||size>PREVIEW_MAX_AU||!duration)return -EINVAL;
    struct {const uint8_t *data;size_t length;} nal[128];
    size_t count=0,total=0,length=0,at=prefix(au,size,0,&length);int vcl=0,idr=0;
    if(at==size)return -EBADMSG;
    for(size_t i=0;i<at;i++)if(au[i])return -EBADMSG;
    while(at<size) {
        size_t start=at+length,next_length=0,next=prefix(au,size,start+1,&next_length);
        if(start>=size||(au[start]&0x80)||!(au[start]&31)||(au[start]&31)>23)return -EBADMSG;
        unsigned kind=au[start]&31;
        if(kind==1||kind==5)vcl=1;
        if(kind==5)idr=1;
        /* avc1 configuration is out-of-band in avcC. */
        if(kind!=7&&kind!=8) {
            if(count==128||next-start>UINT32_MAX)return -E2BIG;
            nal[count].data=au+start;nal[count++].length=next-start;total+=4+next-start;
        }
        at=next;length=next_length;
    }
    if(!vcl||idr!=!!key||total>UINT32_MAX-8)return -EBADMSG;
    struct writer w={out,0,capacity,0};size_t moof=box(&w,"moof"),b=box(&w,"mfhd");
    u32(&w,0);u32(&w,sequence);finish(&w,b);
    size_t traf=box(&w,"traf");b=box(&w,"tfhd");u32(&w,0x020000);u32(&w,1);finish(&w,b);
    b=box(&w,"tfdt");u32(&w,0x01000000);u64(&w,decode_time);finish(&w,b);
    b=box(&w,"trun");u32(&w,0x00000701);u32(&w,1);size_t offset=w.used;u32(&w,0);
    u32(&w,duration);u32(&w,(uint32_t)total);u32(&w,key?0x02000000:0x01010000);finish(&w,b);finish(&w,traf);finish(&w,moof);
    if(w.failed)return -ENOSPC;
    uint32_t data_offset=(uint32_t)w.used+8;out[offset]=(uint8_t)(data_offset>>24);out[offset+1]=(uint8_t)(data_offset>>16);out[offset+2]=(uint8_t)(data_offset>>8);out[offset+3]=(uint8_t)data_offset;
    b=box(&w,"mdat");for(size_t i=0;i<count;i++){u32(&w,(uint32_t)nal[i].length);bytes(&w,nal[i].data,nal[i].length);}finish(&w,b);
    if(w.failed)return -ENOSPC;
    *used=w.used;return 0;
}
