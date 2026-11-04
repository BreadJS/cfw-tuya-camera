#include "receipt.h"
#include "capsule.h"
#include <string.h>
struct parser {const char *p,*end;};
static void space(struct parser *p){while(p->p<p->end&&(*p->p==' '||*p->p=='\n'||*p->p=='\r'||*p->p=='\t'))p->p++;}
static int take(struct parser *p,char c){space(p);if(p->p==p->end||*p->p++!=c)return -1;
 return 0;
 }
static int string(struct parser *p,char *out,size_t n)
{
 if(take(p,'"'))return -1;
 size_t used=0;
 while(p->p<p->end&&*p->p!='"'){unsigned char c=(unsigned char)*p->p++;
  if(c<32||c>126||c=='\\'||used+1>=n)return -1;
 out[used++]=(char)c;}
 if(p->p==p->end)return -1;
 p->p++;out[used]=0;return 0;
 
}
static int number(struct parser *p,uint64_t *out)
{
 space(p);uint64_t v=0;const char *start=p->p;
 while(p->p<p->end&&*p->p>='0'&&*p->p<='9'){unsigned d=(unsigned)(*p->p++-'0');if(v>(UINT64_MAX-d)/10)return -1;
 v=v*10+d;}
 if(start==p->p||(p->p-start>1&&*start=='0'))return -1;
 *out=v;return 0;
 
}
static int truth(struct parser *p,unsigned *out)
{space(p);if(p->end-p->p>=4&&!memcmp(p->p,"true",4)){p->p+=4;*out=1;return 0;
 }return -1;
 }
int anyka_update_receipt_parse(const char *data,size_t size,int health,struct anyka_update_receipt *out)
{
 if(!data||!out||size>1024)return -1;
 memset(out,0,sizeof *out);
 struct parser p={data,data+size};unsigned seen=0;int first=1;
 if(take(&p,'{'))return -1;
 
 for(;;){space(&p);if(p.p<p.end&&*p.p=='}'){p.p++;break;}if(!first&&take(&p,','))return -1;
 first=0;
  char key[32];if(string(&p,key,sizeof key)||take(&p,':'))return -1;
 unsigned bit=0;int bad=0;
  if(!strcmp(key,"schema")){bit=1;uint64_t v;bad=number(&p,&v)||v!=1;out->schema=1;}
  else if(!strcmp(key,"sequence")){bit=2;bad=number(&p,&out->sequence)||!out->sequence;}
  else if(!strcmp(key,"board_id")){bit=4;bad=string(&p,out->board,sizeof out->board);}
  else if(!strcmp(key,"payload_blake2b")){bit=8;bad=string(&p,out->digest,sizeof out->digest);}
  else if(!strcmp(key,"status")&&!health){bit=16;bad=string(&p,out->status,sizeof out->status);}
  else if(!strcmp(key,"install_boot_id")&&!health){bit=32;bad=string(&p,out->install_boot_id,sizeof out->install_boot_id);}
  else if(!strcmp(key,"boot_id")&&health){bit=16;bad=string(&p,out->boot_id,sizeof out->boot_id);}
  else if(!strcmp(key,"media")&&health){bit=32;bad=truth(&p,&out->media);}
  else if(!strcmp(key,"web")&&health){bit=64;bad=truth(&p,&out->web);}
  else if(!strcmp(key,"network")&&health){bit=128;bad=truth(&p,&out->network);}
  else if(!strcmp(key,"app_root_boot")&&health){bit=256;bad=truth(&p,&out->app_root_boot);}
  else return -1;
 
  if(bad||(seen&bit))return -1;
 seen|=bit;
 }
 space(&p);if(p.p!=p.end||seen!=(health?511u:63u)||strcmp(out->board,ANYKA_UPDATE_BOARD)||strlen(out->digest)!=128)return -1;
 
 for(unsigned i=0;i<128;i++)if(!((out->digest[i]>='0'&&out->digest[i]<='9')||(out->digest[i]>='a'&&out->digest[i]<='f')))return -1;
 
 const char *id=health?out->boot_id:out->install_boot_id;
 if(strlen(id)!=36)return -1;
 
 for(unsigned i=0;i<36;i++)if(i==8||i==13||i==18||i==23){if(id[i]!='-')return -1;
 }else if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')))return -1;
 
 return health||!strcmp(out->status,"written")||!strcmp(out->status,"installing")?0:-1;
}
int anyka_update_can_confirm(const struct anyka_update_receipt *p,
 const struct anyka_update_receipt *h,const char *boot,const char *digest,uint64_t accepted)
{
 if(!p||!h||!boot||!digest||p->schema!=1||h->schema!=1||
    strcmp(p->board,ANYKA_UPDATE_BOARD)||strcmp(h->board,ANYKA_UPDATE_BOARD)||
    !p->sequence||p->sequence<h->sequence||p->sequence!=h->sequence||p->sequence<accepted||
    strcmp(p->status,"written")||strcmp(p->digest,h->digest)||strcmp(p->digest,digest)||
    strcmp(h->boot_id,boot)||!strcmp(p->install_boot_id,boot)||
    !h->media||!h->web||!h->network||!h->app_root_boot)return -1;
 return 0;
}
