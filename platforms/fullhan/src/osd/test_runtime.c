#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
struct fake { int allocations,maps,submits,lease,alloc_error,map_error,submit_error;uint8_t *memory;uint32_t packet[147]; };
static struct fake fake;
int legacy_vmm_allocate(struct legacy_vmm *v,const char *name,uint32_t size,uint32_t alignment,size_t *handle)
{
    assert(!strcmp(name,"clock-osd") && size==4096 && alignment==LEGACY_VMM_PAGE);
    ++fake.allocations;if(fake.alloc_error)return fake.alloc_error;
    assert(v->count<v->capacity);*handle=v->count++;
    v->blocks[*handle]=(struct legacy_vmm_block){.phys=0xa3700000,.size=4096,.allocated=1};return 0;
}
int legacy_vmm_map(struct legacy_vmm *v,size_t handle,enum legacy_vmm_map_method method)
{
    assert(method==LEGACY_VMM_MAP_DIRECT && handle<v->count);++fake.maps;
    if(fake.map_error)return fake.map_error;
    v->blocks[handle].user=(uint32_t)(uintptr_t)fake.memory;v->blocks[handle].mapped=1;return 0;
}
static int check(void *opaque) { assert(opaque==&fake);return fake.lease?0:-EPERM; }
static int submit(void *opaque,int fd,uint32_t request,uint32_t wire[147])
{
    assert(opaque==&fake && fd==77 && request==0xc448696d);
    ++fake.submits;memcpy(fake.packet,wire,sizeof fake.packet);return fake.submit_error;
}
int main(void)
{
    int flags=MAP_PRIVATE|MAP_ANONYMOUS;
#ifdef MAP_32BIT
    flags|=MAP_32BIT;
#endif
    uint8_t *memory=mmap(NULL,4096,PROT_READ|PROT_WRITE,flags,-1,0);
    assert(memory!=MAP_FAILED && (uintptr_t)memory<=UINT32_MAX);
    fake=(struct fake){.memory=memory,.lease=1};
    struct legacy_vmm_block blocks[4];struct legacy_vmm v={.blocks=blocks,.capacity=4};
    struct camera_osd_io io={&fake,check,submit};struct camera_osd osd={0};
    assert(!camera_osd_init(&osd,77,&v,&io,1920,1080));
    assert(fake.allocations==1 && fake.maps==1 && !fake.submits && !osd.enabled);
    assert(camera_osd_init(&osd,77,&v,&io,1920,1080)==-EALREADY);
    assert(camera_osd_set_enabled(&osd,1)==-ENODATA);
    assert(!camera_osd_update_text(&osd,"2026-10-29 Thu 14:07:09"));
    unsigned nonzero=0;for(unsigned i=0;i<2912;i++)nonzero+=!!memory[i];assert(nonzero>100);
    assert(!camera_osd_set_enabled(&osd,1) && fake.submits==1 && fake.packet[2]==1);
    uint8_t previous[2912];memcpy(previous,memory,sizeof previous);
    assert(!camera_osd_update_text(&osd,"2026-10-29 Thu 14:07:09") && !memcmp(previous,memory,sizeof previous));
    assert(!camera_osd_update_text(&osd,"2026-10-29 Thu 14:07:10") && memcmp(previous,memory,sizeof previous));
    assert(fake.submits==1 && fake.allocations==1 && fake.maps==1);
    assert(!camera_osd_set_enabled(&osd,0) && fake.packet[2]==0 && fake.submits==2);
    fake.submit_error=-71;
    assert(camera_osd_set_enabled(&osd,1)==-71 && osd.fault && osd.attempted);
    fake.submit_error=0;assert(!camera_osd_disable(&osd) && !osd.attempted && !osd.enabled);
    assert(camera_osd_set_enabled(&osd,1)==-EPIPE && fake.allocations==1);
    fake.lease=0;assert(camera_osd_disable(&osd)==-EPERM);fake.lease=1;
    struct camera_osd failed={0};fake.map_error=-72;
    assert(camera_osd_init(&failed,77,&v,&io,1920,1080)==-72 && failed.fault && failed.allocated && !failed.mapped);
    assert(v.count==2); /* Failed mapping still retains the actual allocation. */
    assert(!munmap(memory,4096));
    puts("OSD runtime ownership/lifecycle/failure tests PASS; injected calls only");return 0;
}
