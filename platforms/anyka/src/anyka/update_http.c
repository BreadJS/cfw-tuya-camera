#define _GNU_SOURCE
#include "update_http.h"
#include "../update/capsule.h"
#include "../update/receipt.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* The production SDK host itself is LD_PRELOAD-ed. Never pass its constructor
 * environment to an updater, shell or reboot executable. */
static char *const child_environment[]={"PATH=/bin:/sbin:/usr/bin:/usr/sbin","LANG=C",NULL};
struct anyka_update_http {
 pthread_mutex_t lock;char installer[512],directory[512],capsule[600],log[600];
 int dir,app_root_boot,busy,verified,failed,written,confirmed,staging_owned;
 uint64_t sequence,accepted;char digest[129];unsigned blocks;
};
static int private_ram_dir(int fd)
{struct stat s;struct statfs f;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid||(s.st_mode&077)||fstatfs(fd,&f)||
 ((unsigned long)f.f_type!=0x01021994UL&&(unsigned long)f.f_type!=0x858458f6UL)?-1:0;}
static int regular(int fd)
{struct stat s;return fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid||s.st_nlink!=1||(s.st_mode&022)?-1:0;}
static int write_all(int fd,const void *data,size_t n)
{const char *p=data;while(n){ssize_t got=write(fd,p,n);if(got<0&&errno==EINTR)continue;if(got<=0)return -1;p+=got;n-=(size_t)got;}return 0;}
static void reply(struct control_reply *r,unsigned status,const char *text)
{r->status=status;snprintf(r->json,sizeof r->json,"{\"error\":\"%s\"}",text);}
static void release_staging(struct anyka_update_http *c)
{
 if(c->staging_owned){
  if(unlinkat(c->dir,"candidate.new",0)&&errno!=ENOENT)return;
  if(unlinkat(c->dir,"candidate.cap",0)&&errno!=ENOENT)return;
  if(!unlinkat(c->dir,"staging.lock",AT_REMOVEDIR))c->staging_owned=0;}
}
static int run(struct anyka_update_http *c,const char *mode)
{
 long maximum=sysconf(_SC_OPEN_MAX);if(maximum<3||maximum>1048576)return -1;
 int log=openat(c->dir,"installer.log",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
 if(log<0||regular(log)){if(log>=0)close(log);return -1;}
 char *args[]={c->installer,(char*)mode,c->capsule,NULL};if(!strcmp(mode,"--confirm-boot"))args[2]=NULL;
 pid_t child=fork();if(child==0){if(dup2(log,1)<0||dup2(log,2)<0)_exit(126);
  /* Do not retain APP files, SDK handles or listening sockets in the helper.
   * Parent keeps its media descriptors; closing forked copies cannot perform
   * the final driver release while that owner retains them. */
  for(int descriptor=3;descriptor<maximum;descriptor++)close(descriptor);
  execve(c->installer,args,child_environment);_exit(127);}
 close(log);if(child<0)return -1;int status;
 while(waitpid(child,&status,0)<0){if(errno!=EINTR)return -1;}
 return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
static void progress(struct anyka_update_http *c)
{
 int fd=openat(c->dir,"installer.log",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return;
 struct stat s;char text[4097];if(fstat(fd,&s)||regular(fd)){close(fd);return;}
 off_t pos=s.st_size>4096?s.st_size-4096:0;ssize_t n=pread(fd,text,4096,pos);close(fd);if(n<=0)return;text[n]=0;
 const char *p=text;while((p=strstr(p,"blocks_verified="))){unsigned blocks,total;if(sscanf(p,"blocks_verified=%u/%u",&blocks,&total)==2&&total==1280&&blocks<=1280)c->blocks=blocks;p++;}
}
static void status_locked(struct anyka_update_http *c,struct control_reply *r)
{
 progress(c);r->status=200;
 const char *phase=c->busy?"installing":c->failed?"failed":c->written?"written_reboot_pending":"idle";
 snprintf(r->json,sizeof r->json,"{\"available\":true,\"accepted\":%"PRIu64",\"pending\":%"PRIu64",\"sequence\":%"PRIu64",\"verified_pending\":%s,\"signed_pending_record\":%s,\"staged\":%s,\"payload_digest\":\"%s\",\"install_available\":%s,\"discard_available\":%s,\"installation_phase\":\"%s\",\"blocks_completed\":%u,\"blocks_total\":1280,\"installation_progress\":{\"blocks_completed\":%u,\"blocks_total\":1280},\"boot_confirmed\":%s,\"confirmed_sequence\":%"PRIu64",\"applied\":false}",
 c->accepted,c->sequence,c->sequence,c->verified?"true":"false",c->verified?"true":"false",c->verified?"true":"false",c->digest,
 c->app_root_boot&&!c->busy&&!c->written?"true":"false",!c->busy&&!c->written?"true":"false",phase,c->blocks,c->blocks,c->confirmed?"true":"false",c->accepted);
}
static int marker(struct anyka_update_http *c)
{
 char a[64],b[64];int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);
 if(fd<0)return -1;
 ssize_t n=read(fd,a,sizeof a);close(fd);
 fd=openat(c->dir,"quiesced",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 ssize_t m=read(fd,b,sizeof b);int bad=regular(fd)||n!=37||m!=n||memcmp(a,b,(size_t)(n==37?n:0));close(fd);return bad?-1:0;
}
static void delay(unsigned milliseconds)
{struct timespec t={milliseconds/1000,(long)(milliseconds%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static void *install_worker(void *opaque)
{
 struct anyka_update_http *c=opaque;delay(2000);int bad=kill(getpid(),SIGUSR1);unsigned waited=0;
 while(!bad&&marker(c)){if(waited++>=350){bad=1;break;}delay(100);}
 if(!bad)bad=run(c,"--apply");
 pthread_mutex_lock(&c->lock);c->busy=0;c->failed=bad!=0;c->written=!bad;pthread_mutex_unlock(&c->lock);
 if(!bad){delay(1000);pid_t child=fork();if(child==0){char *args[]={"/sbin/reboot","-f",NULL};execve(args[0],args,child_environment);_exit(127);}if(child>0){int s;while(waitpid(child,&s,0)<0&&errno==EINTR){}}}
 return NULL;
}
int anyka_update_http_open(struct anyka_update_http **out,const struct anyka_update_http_options *o)
{
 if(!out||*out||!o||!o->installer||!o->runtime_directory||strlen(o->installer)>=512||strlen(o->runtime_directory)>=512)return -1;
 struct anyka_update_http *c=calloc(1,sizeof *c);if(!c)return -1;
 strcpy(c->installer,o->installer);strcpy(c->directory,o->runtime_directory);c->app_root_boot=o->app_root_boot;
 c->dir=open(c->directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int fd=open(c->installer,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct statfs fs;
 if(c->dir<0||private_ram_dir(c->dir)||fd<0||regular(fd)||fstatfs(fd,&fs)||
 ((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)){if(fd>=0)close(fd);if(c->dir>=0)close(c->dir);free(c);return -1;}
 close(fd);snprintf(c->capsule,sizeof c->capsule,"%s/candidate.cap",c->directory);snprintf(c->log,sizeof c->log,"%s/installer.log",c->directory);
 if(pthread_mutex_init(&c->lock,NULL)){close(c->dir);free(c);return -1;}
 FILE *accepted=fopen("/etc/config/camera/update/accepted","r");if(accepted){if(fscanf(accepted,"%"SCNu64,&c->accepted)!=1)c->accepted=0;fclose(accepted);}*out=c;return 0;
}
void anyka_update_http_backend(void *opaque,const struct control_request *q,struct control_reply *r)
{
 struct anyka_update_http *c=opaque;if(!c||!q||!r)return;
 if(strncmp(q->path,"/api/update",11)){reply(r,404,"unknown_update_route");return;}
 pthread_mutex_lock(&c->lock);
 if(!strcmp(q->path,"/api/update/status")&&!strcmp(q->method,"GET")){status_locked(c,r);goto done;}
 if(strcmp(q->method,"POST")){reply(r,405,"POST_required");goto done;}
 if(c->busy||c->written){reply(r,409,"installation_busy_or_restart_required");goto done;}
 if(!strcmp(q->path,"/api/update")){
  if(q->body.length!=ANYKA_UPDATE_HEADER+ANYKA_APP_BYTES||!q->body.read){reply(r,413,"exact_APP_capsule_size_required");goto done;}
  if(!c->staging_owned){if(mkdirat(c->dir,"staging.lock",0700)){reply(r,409,"another_update_owns_RAM_staging");goto done;}c->staging_owned=1;}
  /* Only one full capsule fits with the live SDK. A replacement upload
   * intentionally discards the previous staged candidate before allocating. */
  if(unlinkat(c->dir,"candidate.cap",0)&&errno!=ENOENT){reply(r,500,"old_candidate_removal_failed");goto done;}
  c->verified=0;c->sequence=0;c->digest[0]=0;c->failed=0;c->blocks=0;
  int fd=openat(c->dir,"candidate.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0){release_staging(c);reply(r,409,"staging_file_exists");goto done;}
  uint8_t buffer[4096];uint64_t left=q->body.length;int bad=0;
  while(left){size_t want=left<sizeof buffer?(size_t)left:sizeof buffer;ssize_t n=q->body.read(q->body.opaque,buffer,want);
   if(n<=0||(size_t)n>want||write_all(fd,buffer,(size_t)n)){bad=1;break;}left-=(size_t)n;}
  if(!bad&&(fchmod(fd,0400)||fsync(fd)))bad=1;
  if(close(fd))bad=1;
  if(!bad&&renameat(c->dir,"candidate.new",c->dir,"candidate.cap"))bad=1;
  if(bad){release_staging(c);reply(r,400,"upload_incomplete");goto done;}
  if(run(c,"--verify")){release_staging(c);reply(r,403,"signed_APP_verification_failed");goto done;}
  fd=openat(c->dir,"candidate.cap",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);uint8_t header[192];
  bad=fd<0||regular(fd)||pread(fd,header,sizeof header,0)!=(ssize_t)sizeof header;if(fd>=0)close(fd);
  if(bad){release_staging(c);reply(r,500,"verified_candidate_unreadable");goto done;}
  for(unsigned i=0;i<8;i++)c->sequence|=(uint64_t)header[48+i]<<(i*8);
  anyka_update_hex(c->digest,header+64);c->verified=1;status_locked(c,r);
 }else if(!strcmp(q->path,"/api/update/plan")){
  if(!c->verified){reply(r,409,"verified_upload_required");goto done;}
  r->status=200;snprintf(r->json,sizeof r->json,"{\"sequence\":%"PRIu64",\"kernel_selected\":false,\"rootfs_selected\":false,\"app_selected\":true,\"write_bytes\":5242880,\"blocks_total\":1280,\"protected_partitions_preserved\":true}",c->sequence);
 }else if(!strcmp(q->path,"/api/update/install")){
  char body[512],confirm[16],sequence[32],digest[129],expected[32];snprintf(expected,sizeof expected,"%"PRIu64,c->sequence);
  if(!c->verified||!c->app_root_boot||control_body_text(&q->body,body,sizeof body)||
   control_form(body,"confirm",confirm,sizeof confirm)!=1||strcmp(confirm,"install")||
   control_form(body,"sequence",sequence,sizeof sequence)!=1||strcmp(sequence,expected)||
   control_form(body,"payload_digest",digest,sizeof digest)!=1||strcmp(digest,c->digest)){
   reply(r,409,"explicit_matching_install_confirmation_and_APP_boot_required");goto done;}
  if(run(c,"--verify")){reply(r,403,"candidate_reverification_failed");goto done;}
  /* A quiesced owner never resumes SDK/DMA execution before reboot. Retain
   * its valid receipt on repair retries; deleting it would strand the owner
   * after its one acknowledged handoff. */
  if(marker(c))unlinkat(c->dir,"quiesced",0);
  c->busy=1;c->failed=0;c->blocks=0;pthread_t worker;
  if(pthread_create(&worker,NULL,install_worker,c)){c->busy=0;reply(r,500,"install_worker_failed");goto done;}pthread_detach(worker);
  status_locked(c,r);r->status=202;
 }else if(!strcmp(q->path,"/api/update/cancel")||!strcmp(q->path,"/api/update/discard")){
  release_staging(c);
  if(c->staging_owned){reply(r,500,"discard_failed");goto done;}
  c->verified=0;c->sequence=0;c->digest[0]=0;c->failed=0;status_locked(c,r);
 }else reply(r,404,"unknown_update_route");
 done:pthread_mutex_unlock(&c->lock);
}
int anyka_update_http_health(struct anyka_update_http *c,int media,int web,int network,int app_boot)
{
 if(!c||!media||!web||!network||!app_boot||!c->app_root_boot)return -1;
 pthread_mutex_lock(&c->lock);if(c->busy||c->confirmed){pthread_mutex_unlock(&c->lock);return c->confirmed?0:-1;}
 int dir=open("/etc/config/camera/update",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 int fd=dir<0?-1:openat(dir,"pending.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);char json[1024];ssize_t n=fd<0?-1:read(fd,json,sizeof json);struct anyka_update_receipt p;
 int bad=fd<0||regular(fd)||n<1||anyka_update_receipt_parse(json,(size_t)(n>0?n:0),0,&p)||strcmp(p.status,"written");if(fd>=0)close(fd);
 char boot[64];fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);n=fd<0?-1:read(fd,boot,sizeof boot);if(fd>=0)close(fd);
 if(bad||n!=37){if(dir>=0)close(dir);pthread_mutex_unlock(&c->lock);return -1;}boot[36]=0;
 n=snprintf(json,sizeof json,"{\"schema\":1,\"board_id\":\"%s\",\"sequence\":%"PRIu64",\"payload_blake2b\":\"%s\",\"boot_id\":\"%s\",\"media\":true,\"web\":true,\"network\":true,\"app_root_boot\":true}\n",ANYKA_UPDATE_BOARD,p.sequence,p.digest,boot);
 fd=openat(dir,"boot-health.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 bad=fd<0||write_all(fd,json,(size_t)n)||fsync(fd);if(fd>=0&&close(fd))bad=1;
 if(!bad&&(renameat(dir,"boot-health.new",dir,"boot-health.json")||fsync(dir)))bad=1;
 if(!bad)bad=run(c,"--confirm-boot");
 if(!bad){c->confirmed=1;c->accepted=p.sequence;}
 close(dir);pthread_mutex_unlock(&c->lock);return bad?-1:0;
}
