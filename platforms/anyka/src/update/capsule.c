#include "capsule.h"
#include "vendor/monocypher.h"
#include "vendor/monocypher-ed25519.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p)
{return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
void anyka_update_digest(uint8_t out[64],const void *p,size_t n)
{crypto_blake2b(out,64,p,n);}
void anyka_update_hex(char out[129],const uint8_t digest[64])
{static const char h[]="0123456789abcdef";for(unsigned i=0;i<64;i++){out[i*2]=h[digest[i]>>4];out[i*2+1]=h[digest[i]&15];}out[128]=0;}
int anyka_update_verify(const uint8_t *p,size_t n,const uint8_t key[32],
                        uint64_t accepted,struct anyka_update_verified *v)
{
 static const uint8_t magic[8]={'A','K','C','A','M','O','1',0};
 static const uint8_t board[32]=ANYKA_UPDATE_BOARD;
 if(!p||!key||!v||n!=ANYKA_UPDATE_HEADER+ANYKA_APP_BYTES)return -1;
 if(memcmp(p,magic,8)||le32(p+8)!=1||le32(p+12)!=ANYKA_UPDATE_HEADER||
    memcmp(p+16,board,32)||le64(p+56)!=ANYKA_APP_BYTES)return -1;
 uint64_t sequence=le64(p+48);
 if(!sequence||sequence<=accepted||crypto_ed25519_check(p+128,key,p,128))return -1;
 uint8_t digest[64];anyka_update_digest(digest,p+ANYKA_UPDATE_HEADER,ANYKA_APP_BYTES);
 if(crypto_verify64(digest,p+64))return -1;
 const uint8_t *app=p+ANYKA_UPDATE_HEADER;
 /* The image builder must emit SquashFS4. Its declared used bytes must fit
  * APP; all unused flash is explicitly ff and covered by the signature. */
 uint64_t used=le64(app+40);
 if(memcmp(app,"hsqs",4)||app[28]!=4||app[29]!=0||used<96||used>ANYKA_APP_BYTES)return -1;
 for(size_t i=(size_t)used;i<ANYKA_APP_BYTES;i++)if(app[i]!=0xff)return -1;
 v->sequence=sequence;memcpy(v->digest,digest,64);return 0;
}
int anyka_update_apply(const uint8_t *p,size_t n,const uint8_t key[32],
                       uint64_t accepted,const struct anyka_update_flash *f)
{
 struct anyka_update_verified v;
 if(!f||!f->erase||!f->write||!f->read||!f->progress||
    anyka_update_verify(p,n,key,accepted,&v))return -1;
 uint8_t candidate[ANYKA_APP_ERASE],readback[ANYKA_APP_ERASE];
 if(f->progress(f->context,0,ANYKA_APP_BYTES/ANYKA_APP_ERASE))return -1;
 for(uint32_t offset=0;offset<ANYKA_APP_BYTES;offset+=ANYKA_APP_ERASE){
  /* Keep the active block private before erasing. The production backend
   * holds a read lease on the immutable RAM source and checks it per erase. */
  memcpy(candidate,p+ANYKA_UPDATE_HEADER+offset,sizeof candidate);
  const uint8_t *block=candidate;
  if(f->erase(f->context,offset,ANYKA_APP_ERASE)||
     f->write(f->context,offset,block,ANYKA_APP_ERASE)||
     f->read(f->context,offset,readback,sizeof readback)||
     memcmp(block,readback,sizeof readback)||
     f->progress(f->context,offset/ANYKA_APP_ERASE+1,ANYKA_APP_BYTES/ANYKA_APP_ERASE))return -1;
 }
 return 0;
}
