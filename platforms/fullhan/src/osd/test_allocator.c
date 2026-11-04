#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static unsigned allocations,maps,submits;
static uint8_t *memory;
static int allocation_error,map_error;
static int lease(void *p){(void)p;return 0;}
static int open_device(void *p,int *fd){(void)p;*fd=42;return 0;}
static int close_device(void *p,int fd,int *consumed){(void)p;(void)fd;(void)consumed;assert(!"OSD must retain live VMM resources");return -EPERM;}
static int alloc_ioctl(void *p,int fd,uint32_t cmd,uint8_t wire[76]){
 (void)p;assert(fd==42&&cmd==LEGACY_VMM_ALLOC);struct legacy_vmm_request r;legacy_vmm_decode(wire,&r);
 assert(r.alignment==LEGACY_VMM_PAGE&&r.size==CAMERA_OSD_STORAGE&&!strcmp(r.name,"clock-osd"));
 allocations++;if(allocation_error)return allocation_error;r.phys=0xa3700000;legacy_vmm_encode(&r,wire);return 0;
}
static int map_memory(void *p,int fd,uint32_t phys,uint32_t size,unsigned prot,unsigned flags,uint32_t *user){
 (void)p;assert(fd==42&&phys==0xa3700000&&size==4096&&prot==3&&flags==1);maps++;if(map_error)return map_error;*user=(uint32_t)(uintptr_t)memory;return 0;
}
static int unmap_memory(void *p,uint32_t user,uint32_t size){(void)p;(void)user;(void)size;assert(!"OSD must not unmap live backing");return -EPERM;}
static int submit(void *p,int fd,uint32_t cmd,uint32_t wire[147]){(void)p;assert(fd==77&&cmd==CAMERA_GRAPHV2_SET);assert(wire[4]==0xa3700000||!wire[2]);submits++;return 0;}
int main(void){
 int flags=MAP_PRIVATE|MAP_ANONYMOUS;
#ifdef MAP_32BIT
 flags|=MAP_32BIT;
#endif
 memory=mmap(NULL,4096,PROT_READ|PROT_WRITE,flags,-1,0);assert(memory!=MAP_FAILED&&(uintptr_t)memory<=UINT32_MAX);
 struct legacy_vmm_ops ops={lease,lease,open_device,close_device,alloc_ioctl,map_memory,unmap_memory};
 struct legacy_vmm_block blocks[4];struct legacy_vmm v;assert(!legacy_vmm_init(&v,&ops,NULL,blocks,4));assert(!legacy_vmm_open(&v));
 struct camera_osd_io io={NULL,lease,submit};struct camera_osd osd={0};
 assert(!camera_osd_init(&osd,77,&v,&io,1920,1080));assert(allocations==1&&maps==1&&v.count==1);
 assert(!camera_osd_update_text(&osd,"2026-10-29 Thu 01:10:00"));assert(!camera_osd_set_enabled(&osd,1));assert(submits==1);
 assert(!camera_osd_update_text(&osd,"2026-10-29 Thu 01:10:01"));assert(!camera_osd_set_enabled(&osd,0));assert(submits==2&&allocations==1&&maps==1);
 /* Real allocator must keep rejecting the original invalid alignment. */
 size_t handle;assert(legacy_vmm_allocate(&v,"bad-alignment",4096,128,&handle)==-EINVAL&&allocations==1);
 /* Real map failures preserve their tracked allocation and remain optional. */
 struct legacy_vmm failed;struct legacy_vmm_block fb[1];assert(!legacy_vmm_init(&failed,&ops,NULL,fb,1));assert(!legacy_vmm_open(&failed));
 struct camera_osd failed_osd={0};map_error=-ENOMEM;assert(camera_osd_init(&failed_osd,77,&failed,&io,1920,1080)==-ENOMEM);assert(failed_osd.allocated&&failed_osd.fault&&failed.count==1);
 assert(!munmap(memory,4096));puts("OSD + real VMM allocator: page alignment, mapping, enable/tick/disable and retained failure PASS");return 0;
}
