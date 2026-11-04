#include "../../src/update/capsule.h"
#include "../../src/update/receipt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct memory {unsigned erased,written,read,progress;int fail;uint8_t *flash;};
static int erase(void *o,uint32_t off,uint32_t n)
{struct memory *m=o;assert(off==m->erased*4096&&n==4096&&off+n<=ANYKA_APP_BYTES);m->erased++;memset(m->flash+off,255,n);return m->fail==1?-1:0;}
static int write_block(void *o,uint32_t off,const void *p,size_t n)
{struct memory *m=o;assert(m->erased==m->written+1&&off==m->written*4096&&n==4096);m->written++;memcpy(m->flash+off,p,n);return m->fail==2?-1:0;}
static int read_block(void *o,uint32_t off,void *p,size_t n)
{struct memory *m=o;assert(m->written==m->read+1&&off==m->read*4096&&n==4096);m->read++;memcpy(p,m->flash+off,n);if(m->fail==3)((uint8_t*)p)[n-1]^=1;return 0;}
static int progress(void *o,unsigned done,unsigned total)
{struct memory *m=o;assert(total==1280&&done==m->progress&&done==m->read);m->progress++;return m->fail==4?-1:0;}
static uint8_t *load(const char *path,size_t n)
{FILE *f=fopen(path,"rb");assert(f);uint8_t *p=malloc(n);assert(p&&fread(p,1,n,f)==n&&fgetc(f)==EOF);fclose(f);return p;}
int main(int argc,char **argv)
{
 assert(argc==3);size_t size=ANYKA_UPDATE_HEADER+ANYKA_APP_BYTES;
 uint8_t *capsule=load(argv[1],size),*key=load(argv[2],32);struct anyka_update_verified v;
 assert(!anyka_update_verify(capsule,size,key,0,&v)&&v.sequence==7);
 assert(anyka_update_verify(capsule,size,key,7,&v));
 struct memory m={0};m.flash=malloc(ANYKA_APP_BYTES);assert(m.flash);
 struct anyka_update_flash f={&m,erase,write_block,read_block,progress};
 assert(!anyka_update_apply(capsule,size,key,0,&f)&&m.read==1280&&m.progress==1281);
 assert(!memcmp(m.flash,capsule+ANYKA_UPDATE_HEADER,ANYKA_APP_BYTES));
 for(unsigned i=0;i<4;i++){
  size_t positions[]={0,48,128,ANYKA_UPDATE_HEADER+300};capsule[positions[i]]^=1;
  m.erased=m.written=m.read=m.progress=0;
  assert(anyka_update_apply(capsule,size,key,0,&f)&&!m.erased&&!m.written&&!m.progress);
  capsule[positions[i]]^=1;
 }
 for(int failure=1;failure<=4;failure++){
  m.erased=m.written=m.read=m.progress=0;m.fail=failure;
  assert(anyka_update_apply(capsule,size,key,0,&f));assert(m.erased<=1&&m.written<=1&&m.read<=1);
 }
 char digest[129];anyka_update_hex(digest,v.digest);
 char json[1024];struct anyka_update_receipt r;
 int n=snprintf(json,sizeof json,"{\"schema\":1,\"sequence\":7,\"board_id\":\"%s\",\"payload_blake2b\":\"%s\",\"boot_id\":\"12345678-1234-1234-1234-123456789abc\",\"media\":true,\"web\":true,\"network\":true,\"app_root_boot\":true}",ANYKA_UPDATE_BOARD,digest);
 assert(!anyka_update_receipt_parse(json,(size_t)n,1,&r)&&r.sequence==7&&r.network);
 struct anyka_update_receipt pending=r;
 strcpy(pending.status,"written");strcpy(pending.install_boot_id,"aaaaaaaa-1234-1234-1234-123456789abc");
 assert(!anyka_update_can_confirm(&pending,&r,r.boot_id,digest,6));
 assert(!anyka_update_can_confirm(&pending,&r,r.boot_id,digest,7));
 assert(anyka_update_can_confirm(&pending,&r,r.boot_id,digest,8));
 assert(anyka_update_can_confirm(&pending,&r,pending.install_boot_id,digest,6));
 char wrong[129];memset(wrong,'0',128);wrong[128]=0;
 assert(anyka_update_can_confirm(&pending,&r,r.boot_id,wrong,6));
 pending.sequence++;assert(anyka_update_can_confirm(&pending,&r,r.boot_id,digest,6));pending.sequence--;
 r.network=0;assert(anyka_update_can_confirm(&pending,&r,r.boot_id,digest,6));r.network=1;
 strcpy(pending.status,"installing");assert(anyka_update_can_confirm(&pending,&r,r.boot_id,digest,6));
 json[n-1]=',';strcpy(json+n,"\"media\":true}");assert(anyka_update_receipt_parse(json,strlen(json),1,&r));
 const char *bad="{\"schema\":1,\"schema\":1}";assert(anyka_update_receipt_parse(bad,strlen(bad),1,&r));
 assert(anyka_update_receipt_parse(json,1025,1,&r));
 free(capsule);free(key);free(m.flash);puts("Anyka signed APP verifier/readback/fault ordering and strict receipt schema PASS");return 0;
}
