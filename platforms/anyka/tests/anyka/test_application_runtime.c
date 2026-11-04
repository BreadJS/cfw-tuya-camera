#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <pthread.h>
#include <unistd.h>
/* Root file ownership is represented by a host-only syscall seam. No path,
 * mount classification or RAM proof is bypassed in production code. */
static int test_fstat(int fd,struct stat *s)
{int rc=(int)syscall(SYS_fstat,fd,s);if(!rc&&s->st_uid==getuid())s->st_uid=0;return rc;}
static int test_fstatfs(int fd,struct statfs *s)
{int rc=(int)syscall(SYS_fstatfs,fd,s);if(!rc)s->f_type=0x01021994;return rc;}
#define fstat test_fstat
#define fstatfs test_fstatfs
#include "../../src/anyka/application_runtime.c"
#undef fstat
#undef fstatfs
#include <assert.h>
struct ack_context {char directory[512];unsigned observed;};
static void *acknowledge(void *opaque)
{
 struct ack_context *c=opaque;char request[600],ack[600];
 snprintf(request,sizeof request,"%s/quiesced",c->directory);snprintf(ack,sizeof ack,"%s/quiesced.ack",c->directory);
 for(unsigned attempt=0;attempt<1000;attempt++){
  int fd=open(request,O_RDONLY);char uuid[38];ssize_t n=fd<0?-1:read(fd,uuid,sizeof uuid);if(fd>=0)close(fd);
  if(n==37){c->observed=1;fd=open(ack,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0&&write(fd,uuid,37)==37);close(fd);return NULL;}
  struct timespec wait={0,1000000};nanosleep(&wait,NULL);
 }
 return NULL;
}
int main(int argc,char **argv)
{
 assert(argc==2);
 struct stat st={.st_mode=S_IFREG|0775,.st_uid=0,.st_dev=makedev(31,5)};
 struct statfs fs={.f_type=0x73717368UL,.f_flags=ST_RDONLY};
 assert(preserved_library_metadata("/lib/libc.so.0",&st,&fs));
 st.st_uid=1000;assert(preserved_library_metadata("/lib/libc.so.0",&st,&fs));
 st.st_uid=1001;assert(!preserved_library_metadata("/lib/libc.so.0",&st,&fs));st.st_uid=0;
 assert(!preserved_library_metadata("/usr/lib/libc.so.0",&st,&fs));
 st.st_dev=makedev(31,7);assert(!preserved_library_metadata("/lib/libc.so.0",&st,&fs));st.st_dev=makedev(31,5);
 fs.f_flags=0;assert(!preserved_library_metadata("/lib/libc.so.0",&st,&fs));
 struct stat app={.st_mode=S_IFDIR|0755,.st_uid=0,.st_dev=makedev(31,7)};
 fs.f_flags=ST_RDONLY;assert(app_mount_metadata(&app,&fs)==1);
 app.st_dev=makedev(7,0);assert(app_mount_metadata(&app,&fs)==2);
 app.st_dev=makedev(31,5);assert(!app_mount_metadata(&app,&fs));
 app.st_dev=makedev(31,7);fs.f_flags=0;assert(!app_mount_metadata(&app,&fs));
 fs.f_flags=ST_RDONLY;app.st_uid=1000;assert(!app_mount_metadata(&app,&fs));
 char a[512],b[512];snprintf(a,sizeof a,"%s/a",argv[1]);snprintf(b,sizeof b,"%s/b",argv[1]);
 int fd=open(a,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0&&write(fd,"abcdef",6)==6);close(fd);
 fd=open(b,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0&&write(fd,"abcdef",6)==6);close(fd);
 assert(!same_file_bytes(a,b,6));assert(same_file_bytes(a,b,5));
 fd=open(b,O_WRONLY);assert(fd>=0&&pwrite(fd,"Z",1,5)==1);close(fd);assert(same_file_bytes(a,b,6));
 char id[37];assert(!boot_uuid(id));assert(id[36]=='\n');
 char pidfile[600];snprintf(pidfile,sizeof pidfile,"%s/supervisor.pid",argv[1]);
 fd=open(pidfile,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);char pid[32];int count=snprintf(pid,sizeof pid,"%ld\n",(long)getpid());assert(write(fd,pid,(size_t)count)==count);close(fd);
 setenv("CAMERA_NETWORK_RUNTIME",argv[1],1);struct ack_context ack={0};strcpy(ack.directory,argv[1]);pthread_t thread;
 assert(!pthread_create(&thread,NULL,acknowledge,&ack));assert(!network_quiesce(id));pthread_join(thread,NULL);assert(ack.observed);
 fd=open(pidfile,O_WRONLY|O_TRUNC);assert(fd>=0&&write(fd,"0\n",2)==2);close(fd);assert(network_quiesce(id)==-ESRCH);
 /* Host executable and sanitizer DSOs live on the host filesystem, so they
  * must not produce the camera's RAM-only installation receipt. */
 int rc=anyka_application_ram_safe();assert(rc<0);
 setenv("CAMERA_APP_ROOT_BOOT","1",1);assert(!anyka_application_app_source());
 assert(anyka_application_quiesce_receipt("/tmp/ptz-must-not-create-receipt")<0);
 puts("PASS application runtime: APP mount/byte provenance, network pause acknowledgement, UUID shape, and host-backed receipt refusal");
}
