#include "legacy_vmm.h"
#include <errno.h>
#include <string.h>

static int status(int rc) { return rc > 0 ? -EIO : rc; }
static void put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
void legacy_vmm_encode(const struct legacy_vmm_request *r, uint8_t w[76])
{
    put32(w,r->phys); put32(w+4,r->alignment); put32(w+8,r->size);
    put32(w+12,r->reserved_0c); put32(w+16,r->user); put32(w+20,r->flags);
    memcpy(w+24,r->name,16); memcpy(w+40,r->zone,32); put32(w+72,r->zone_selector);
}
void legacy_vmm_decode(const uint8_t w[76], struct legacy_vmm_request *r)
{
    r->phys=get32(w); r->alignment=get32(w+4); r->size=get32(w+8);
    r->reserved_0c=get32(w+12); r->user=get32(w+16); r->flags=get32(w+20);
    memcpy(r->name,w+24,16); memcpy(r->zone,w+40,32); r->zone_selector=get32(w+72);
}
int legacy_vmm_init(struct legacy_vmm *s, const struct legacy_vmm_ops *ops, void *p,
                    struct legacy_vmm_block *blocks, size_t capacity)
{
    if (!s || !ops || !blocks || !capacity) return -EINVAL;
    if (!ops->acquire || !ops->release || !ops->open || !ops->close ||
        !ops->ioctl || !ops->mmap || !ops->munmap) return -ENOTSUP;
    memset(s,0,sizeof(*s)); s->ops=*ops; s->opaque=p; s->blocks=blocks;
    s->capacity=capacity; s->fd=-1;
    return 0;
}
int legacy_vmm_open(struct legacy_vmm *s)
{
    int rc, fd=-1;
    if (!s || !s->ops.acquire) return -EINVAL;
    if (s->owned || s->fd >= 0 || s->fault) return -EBUSY;
    rc=status(s->ops.acquire(s->opaque));
    if (rc) return rc;
    s->owned=1;
    rc=status(s->ops.open(s->opaque,&fd));
    if (rc || fd<0) {
        /* Failed open callback must leave no descriptor; an output descriptor
         * despite failure is ambiguous and must not be discarded. */
        if (fd>=0) { s->fd=fd; s->fault=s->quarantine=1; return rc ? rc : -EIO; }
        rc=rc ? rc : -EIO;
        if (!status(s->ops.release(s->opaque))) s->owned=0;
        else s->fault=1;
        return rc;
    }
    s->fd=fd;
    return 0;
}
static int live(struct legacy_vmm *s)
{ return !s ? -EINVAL : !s->owned || s->fd<0 || s->fault ? -EPIPE : 0; }
static int valid_phys(uint32_t phys, uint32_t size, uint32_t alignment)
{
    return phys>=LEGACY_VMM_ZONE_START && phys<LEGACY_VMM_ZONE_END &&
        size<=LEGACY_VMM_ZONE_END-phys && !(phys&(alignment-1u));
}
int legacy_vmm_allocate(struct legacy_vmm *s, const char *name, uint32_t size,
                        uint32_t alignment, size_t *handle)
{
    struct legacy_vmm_block *b;
    uint8_t wire[76];
    size_t len, i;
    int rc=live(s);
    if (rc) return rc;
    if (!name || !handle || !size || !alignment || (alignment&(alignment-1u)) ||
        alignment<LEGACY_VMM_PAGE) return -EINVAL;
    for (len=0;len<16 && name[len];++len) {}
    if (!len || len==16) return -ENAMETOOLONG;
    if (size>LEGACY_VMM_ZONE_END-LEGACY_VMM_ZONE_START ||
        alignment>LEGACY_VMM_ZONE_END-LEGACY_VMM_ZONE_START) return -ERANGE;
    if (s->count==s->capacity) return -ENOSPC;
    size=(size+LEGACY_VMM_PAGE-1u)&~(LEGACY_VMM_PAGE-1u);
    b=&s->blocks[s->count]; memset(b,0,sizeof(*b));
    b->size=size; b->request.size=size; b->request.alignment=alignment;
    memcpy(b->request.name,name,len+1);
    memcpy(b->request.zone,"anonymous",10);
    legacy_vmm_encode(&b->request,wire);
    rc=status(s->ops.ioctl(s->opaque,s->fd,LEGACY_VMM_ALLOC,wire));
    legacy_vmm_decode(wire,&b->request);
    if (rc) {
        /* Even ENOMEM can follow successful bump allocation: kmem info
         * allocation fails later. A copy_to_user failure may hide its handle. */
        s->uncertain_allocation=s->fault=1;
        return rc;
    }
    b->phys=b->request.phys; b->allocated=1;
    *handle=s->count++;
    if (!valid_phys(b->phys,size,alignment)) goto invalid;
    for (i=0;i+1<s->count;++i) {
        struct legacy_vmm_block *other=&s->blocks[i];
        if (b->phys<other->phys+other->size && other->phys<b->phys+size) goto invalid;
    }
    return 0;
invalid:
    s->fault=s->quarantine=1;
    return -EPROTO;
}
int legacy_vmm_map(struct legacy_vmm *s, size_t handle, enum legacy_vmm_map_method method)
{
    struct legacy_vmm_block *b;
    struct legacy_vmm_request r;
    uint8_t wire[76];
    uint32_t user=0;
    int rc=live(s);
    if (rc) return rc;
    if (handle>=s->count || (method!=LEGACY_VMM_MAP_IOCTL && method!=LEGACY_VMM_MAP_DIRECT)) return -EINVAL;
    b=&s->blocks[handle];
    if (!b->allocated || b->mapped) return -EBUSY;
    if (method==LEGACY_VMM_MAP_IOCTL) {
        r=b->request; r.phys=b->phys; r.size=b->size; r.user=0;
        /* Exact stock low protection byte=3 and MAP_SHARED flags field=1. */
        r.flags=(r.flags&0xfff00000u)|0x103u;
        legacy_vmm_encode(&r,wire);
        rc=status(s->ops.ioctl(s->opaque,s->fd,LEGACY_VMM_REMAP,wire));
        legacy_vmm_decode(wire,&r);
        if (rc) {
            /* vm_mmap may succeed before copy_to_user fails. No safe retry. */
            s->fault=s->uncertain_mapping=1;
            return rc;
        }
        user=r.user; b->request=r;
    } else {
        rc=status(s->ops.mmap(s->opaque,s->fd,b->phys,b->size,3,1,&user));
        if (rc) { s->fault=1; return rc; }
    }
    b->user=user; b->mapped=1;
    if (!user || (user&(LEGACY_VMM_PAGE-1u)) || user>UINT32_MAX-b->size) {
        s->fault=s->quarantine=1;
        return -EPROTO;
    }
    return 0;
}
int legacy_vmm_allocate_isp(struct legacy_vmm *s, uint32_t size, size_t *handle)
{
    return legacy_vmm_allocate(s,"isp",size,LEGACY_VMM_ISP_ALIGNMENT,handle);
}
int legacy_vmm_unmap(struct legacy_vmm *s, size_t handle)
{
    struct legacy_vmm_block *b;
    int rc;
    if (!s || !s->owned || handle>=s->count) return -EINVAL;
    if (s->quarantine || s->uncertain_mapping) return -ENOTRECOVERABLE;
    b=&s->blocks[handle];
    if (!b->mapped) return 0;
    rc=status(s->ops.munmap(s->opaque,b->user,b->size));
    if (rc) { s->fault=1; return rc; }
    b->mapped=0; b->user=0; b->request.user=0;
    return 0;
}
int legacy_vmm_free(struct legacy_vmm *s, size_t handle)
{
    if (!s || handle>=s->count) return -EINVAL;
    return -ENOTSUP;
}
int legacy_vmm_close(struct legacy_vmm *s)
{
    size_t i;
    int rc, consumed=-1;
    if (!s || !s->ops.release) return -EINVAL;
    if (!s->owned) return 0;
    if (s->quarantine || s->uncertain_mapping || (s->uncertain_allocation && !s->count))
        return -ENOTRECOVERABLE;
    for (i=s->count;i>0;--i) {
        rc=legacy_vmm_unmap(s,i-1);
        if (rc) return rc;
    }
    if (s->fd>=0) {
        rc=status(s->ops.close(s->opaque,s->fd,&consumed));
        if (rc || consumed!=1) {
            s->fault=1;
            if (consumed!=0) { s->quarantine=1; if (consumed==1) s->fd=-1; }
            return rc ? rc : -EPROTO;
        }
        s->fd=-1;
        for (i=0;i<s->count;++i) memset(&s->blocks[i],0,sizeof(s->blocks[i]));
        s->count=0; s->uncertain_allocation=0;
    }
    rc=status(s->ops.release(s->opaque));
    if (rc) { s->fault=1; return rc; }
    s->owned=s->fault=0;
    return 0;
}
