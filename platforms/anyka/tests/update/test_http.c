/* Host syscall seam: private test directory stands in for root-owned tmpfs.
 * Production code has no filesystem bypass or external-program override. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
static int test_fstat(int fd,struct stat *s)
{int rc=(int)syscall(SYS_fstat,fd,s);if(!rc&&s->st_uid==getuid())s->st_uid=0;return rc;}
static int test_fstatfs(int fd,struct statfs *s)
{int rc=(int)syscall(SYS_fstatfs,fd,s);if(!rc)s->f_type=0x01021994;return rc;}
#define fstat test_fstat
#define fstatfs test_fstatfs
#include "../../src/anyka/update_http.c"
#undef fstat
#undef fstatfs
#include <assert.h>
int control_body_text(const struct control_body *b,char *out,size_t n)
{(void)b;(void)out;(void)n;return -1;}
int control_form(const char *body,const char *key,char *out,size_t n)
{(void)body;(void)key;(void)out;(void)n;return -1;}
struct source {uint8_t *p;size_t size,position;};
static ssize_t read_body(void *opaque,void *out,size_t n)
{struct source *s=opaque;if(n>s->size-s->position)n=s->size-s->position;memcpy(out,s->p+s->position,n);s->position+=n;return (ssize_t)n;}
static void request(struct anyka_update_http *c,const char *method,const char *path,struct source *s,size_t length,struct control_reply *r)
{memset(r,0,sizeof *r);struct control_request q={method,path,"application/octet-stream",{s,length,read_body}};anyka_update_http_backend(c,&q,r);}
int main(int argc,char **argv)
{
 assert(argc==3);struct anyka_update_http_options o={argv[1],argv[2],0};struct anyka_update_http *c=NULL;
 /* Setting loader vars now affects child exec only, not this loaded test. */
 setenv("LD_PRELOAD","/no/such/camera-preload.so",1);setenv("LD_LIBRARY_PATH","/no/such/libraries",1);
 assert(!anyka_update_http_open(&c,&o));struct control_reply r;
 request(c,"GET","/api/update/status",NULL,0,&r);assert(r.status==200&&strstr(r.json,"\"install_available\":false"));
 size_t bytes=ANYKA_UPDATE_HEADER+ANYKA_APP_BYTES;uint8_t *data=calloc(1,bytes);assert(data);data[48]=7;data[64]=3;
 struct source s={data,bytes,0};request(c,"POST","/api/update",&s,8,&r);assert(r.status==413&&!s.position);
 assert(!mkdirat(c->dir,"staging.lock",0700));
 request(c,"POST","/api/update",&s,bytes,&r);assert(r.status==409&&!s.position&&!c->staging_owned);
 assert(!unlinkat(c->dir,"staging.lock",AT_REMOVEDIR));
 s.size=bytes-1;request(c,"POST","/api/update",&s,bytes,&r);assert(r.status==400);
 s.size=bytes;s.position=0;request(c,"POST","/api/update",&s,bytes,&r);assert(r.status==200&&c->verified&&c->sequence==7);
 assert(c->staging_owned);s.position=0;data[48]=8;
 request(c,"POST","/api/update",&s,bytes,&r);assert(r.status==200&&c->sequence==8&&c->staging_owned);
 request(c,"POST","/api/update/plan",NULL,0,&r);assert(r.status==200&&strstr(r.json,"\"app_selected\":true")&&strstr(r.json,"\"rootfs_selected\":false"));
 request(c,"POST","/api/update/install",NULL,0,&r);assert(r.status==409&&!c->busy);
 request(c,"POST","/api/update/cancel",NULL,0,&r);assert(r.status==200&&!c->verified&&access(c->capsule,F_OK));
 assert(!c->staging_owned);
 char fail_path[1024];snprintf(fail_path,sizeof fail_path,"%s.fail",argv[1]);int fail=open(fail_path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fail>=0);close(fail);
 s.position=0;request(c,"POST","/api/update",&s,bytes,&r);
 assert(r.status==403&&!c->verified&&access(c->capsule,F_OK));
 assert(anyka_update_http_health(c,1,1,1,0)==-1);
 request(c,"GET","/api/update/nonsense",NULL,0,&r);assert(r.status==405);
 free(data);close(c->dir);pthread_mutex_destroy(&c->lock);free(c);
 puts("Anyka HTTP bounded upload/failed verification/APP-boot install gate/discard PASS");return 0;
}
