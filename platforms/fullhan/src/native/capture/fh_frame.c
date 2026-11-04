#include "fh_frame.h"
#include <errno.h>
#include <string.h>

static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static int span(struct legacy_vmm *v,uint32_t phys,uint32_t user,uint32_t bytes)
{
    size_t i;
    uint64_t delta;
    if (!bytes || !phys || !user) return -EPROTO;
    for (i=0;i<v->count;++i) {
        const struct legacy_vmm_block *b=&v->blocks[i];
        if (!b->allocated || !b->mapped || !b->size || !b->user ||
            b->phys<LEGACY_VMM_ZONE_START || b->phys>=LEGACY_VMM_ZONE_END ||
            b->size>LEGACY_VMM_ZONE_END-b->phys ||
            (uint64_t)b->user+b->size>UINT32_MAX) continue;
        if (phys<b->phys) continue;
        delta=(uint64_t)phys-b->phys;
        if (delta>=b->size || bytes>b->size-delta) continue;
        if ((uint64_t)b->user+delta!=user) continue;
        return 0;
    }
    return -EPROTO;
}
static int validate_copy(struct fh_frame_reader *r,size_t *total)
{
    const uint32_t *q=r->descriptor;
    size_t sum=0,i;
    int rc;
    if (!q[4] || q[4]>FH_FRAME_MAX_ENTRIES) return -EPROTO;
    /* Header extent describes a mapped span; do not reinterpret it as the
     * concatenated AU size or require ring-wrap entries to be contiguous. */
    rc=span(r->capture->vmm,q[6],q[7],q[8]);
    if (rc) return rc;
    for (i=0;i<q[4];++i) {
        size_t base=12+4*i;
        uint32_t len=q[base+3];
        rc=span(r->capture->vmm,q[base+1],q[base+2],len);
        if (rc) return rc;
        if (len>r->capacity-sum) return -ENOSPC;
        sum+=len;
    }
    sum=0;
    for (i=0;i<q[4];++i) {
        size_t base=12+4*i;
        uint32_t len=q[base+3];
        memcpy(r->buffer+sum,(const void *)(uintptr_t)q[base+2],len);
        sum+=len;
    }
    *total=sum;
    return 0;
}
static size_t prefix(const uint8_t *p,size_t n,size_t from,size_t *length)
{
    size_t i;
    for (i=from;i<n && n-i>=3;++i) {
        if (p[i] || p[i+1]) continue;
        if (n-i>=4 && !p[i+2] && p[i+3]==1) { *length=4;return i; }
        if (p[i+2]==1) { *length=3;return i; }
    }
    return n;
}
static int scan(struct fh_frame *f)
{
    size_t position,length=0,next,next_length=0,header,i;
    unsigned type;
    position=prefix(f->data,f->bytes,0,&length);
    if (position==f->bytes) return -EBADMSG;
    for (i=0;i<position;++i) if (f->data[i]) return -EBADMSG;
    while (position<f->bytes) {
        header=position+length;
        if (header==f->bytes || (f->data[header]&0x80)) return -EBADMSG;
        type=f->data[header]&31u;
        if (!type) return -EBADMSG;
        next=prefix(f->data,f->bytes,header+1,&next_length);
        f->nal_mask|=UINT32_C(1)<<type;
        if (type==7 && !f->has_sps) { f->has_sps=1;f->sps.offset=header;f->sps.bytes=next-header; }
        if (type==8 && !f->has_pps) { f->has_pps=1;f->pps.offset=header;f->pps.bytes=next-header; }
        if (type==5) f->has_idr=1;
        if (type>=1 && type<=5) f->has_vcl=1;
        position=next;length=next_length;
    }
    return 0;
}
int fh_frame_reader_init(struct fh_frame_reader *r,struct fh_capture *s,uint8_t *buffer,
                          size_t capacity,const struct fh_frame_clock *clock)
{
    size_t i;
    uintptr_t address=(uintptr_t)buffer;
    if (!r || !s || !s->vmm || !s->io.ioctl || !buffer || !capacity) return -EINVAL;
    if (capacity>UINTPTR_MAX-address) return -ERANGE;
    for (i=0;i<s->vmm->count;++i) {
        const struct legacy_vmm_block *b=&s->vmm->blocks[i];
        if (b->mapped && (uint64_t)address<(uint64_t)b->user+b->size &&
            (uint64_t)b->user<(uint64_t)address+capacity) return -EINVAL;
    }
    memset(r,0,sizeof(*r));
    r->capture=s;r->buffer=buffer;r->capacity=capacity;
    if (clock) r->clock=*clock;
    return 0;
}
int fh_frame_read(struct fh_frame_reader *r,struct fh_frame *out)
{
    uint8_t wire[368]={0},release[4];
    struct fh_frame frame;
    struct fh_capture *s;
    size_t i,total=0;
    int rc,copy_rc,release_rc;
    if (!r || !out) return -EINVAL;
    memset(out,0,sizeof(*out));
    if (r->busy) return -EBUSY;
    if (r->quarantine || r->queue_owned) return -ENOTRECOVERABLE;
    s=r->capture;
    if (!s || !s->vmm || !s->vmm->owned || s->vmm->fd<0 || s->vmm->fault ||
        s->fault || !s->start_ack || !s->bound_ack || s->stop_ack) return -EPIPE;
    r->busy=1;r->release_attempted=0;
    r->last_copy_error=r->last_release_error=r->last_clock_error=0;
    memset(r->descriptor,0,sizeof(r->descriptor));
    put(wire,4);
    rc=s->io.ioctl(s->io.opaque,s->media_fd,FH_CAP_MEDIA_POLL,wire,sizeof(wire));
    r->last_acquire_result=rc;
    if ((uint32_t)rc==FH_FRAME_EMPTY_WIRE) { r->busy=0;return FH_FRAME_EMPTY; }
    if (rc) {
        /* Failure may follow a mutation/failed copyout: do not dequeue again
         * and do not guess which queue would need release. */
        r->ownership_uncertain=r->quarantine=1;r->busy=0;return rc>0 ? -EIO : rc;
    }
    for (i=0;i<FH_FRAME_DESCRIPTOR_WORDS;++i) r->descriptor[i]=get(wire+4*i);
    if (r->descriptor[0]!=4 || r->descriptor[1]!=4 || r->descriptor[2]!=4 ||
        r->descriptor[3]!=s->config.channel) {
        /* A foreign or unidentified queue is not permission to release PAE. */
        r->ownership_uncertain=r->quarantine=1;r->busy=0;return -EPROTO;
    }
    r->queue_owned=1;
    copy_rc=validate_copy(r,&total);
    r->last_copy_error=copy_rc;
    put(release,s->config.channel);
    r->release_attempted=1;
    release_rc=s->io.ioctl(s->io.opaque,s->pae_fd,FH_CAP_PAE_RELEASE,release,sizeof(release));
    if (release_rc) {
        r->last_release_error=release_rc>0 ? -EIO : release_rc;
        r->ownership_uncertain=r->quarantine=1;r->busy=0;
        return r->last_release_error;
    }
    r->queue_owned=0;
    if (copy_rc) { r->busy=0;return copy_rc; }
    memset(&frame,0,sizeof(frame));
    frame.data=r->buffer;frame.bytes=total;
    frame.channel=r->descriptor[3];frame.entry_count=r->descriptor[4];frame.frame_class=r->descriptor[5];
    frame.raw_pts=(uint64_t)r->descriptor[10]|(uint64_t)r->descriptor[11]<<32;
    rc=scan(&frame);
    if (rc) { r->last_copy_error=rc;r->busy=0;return rc; }
    if (r->clock.raw_to_ns) {
        /* Only application callback runs after hardware lease was cleared. */
        rc=r->clock.raw_to_ns(r->clock.opaque,frame.raw_pts,&frame.timestamp_ns);
        if (rc) { r->last_clock_error=rc>0 ? -EIO : rc;r->busy=0;return r->last_clock_error; }
        frame.timestamp_ns_valid=1;
    }
    *out=frame;r->busy=0;
    return 0;
}
