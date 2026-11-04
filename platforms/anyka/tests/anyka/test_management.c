#define _GNU_SOURCE
#include "../../src/anyka/management.c"
#include <assert.h>
struct input {const char *text;size_t at;};
static ssize_t read_body(void *opaque,void *out,size_t capacity)
{struct input *i=opaque;size_t n=strlen(i->text)-i->at;if(n>capacity)n=capacity;memcpy(out,i->text+i->at,n);i->at+=n;return(ssize_t)n;}
static struct control_reply call(struct anyka_management *m,const char *method,const char *path,const char *body)
{struct input in={body?body:"",0};struct control_request q={method,path,"application/x-www-form-urlencoded",{&in,strlen(in.text),read_body}};struct control_reply r;anyka_management_http_backend(m,&q,&r);return r;}
static unsigned delegated,keyframes;
void camera_request_keyframe(void){keyframes++;}
static void delegate(void *opaque,const struct control_request *q,struct control_reply *r)
{(void)opaque;assert(!strcmp(q->path,"/api/wifi"));delegated++;r->status=200;strcpy(r->json,"{}");}
static void finish(struct anyka_management *m)
{struct anyka_management_command c;assert(anyka_management_take(m,&c)==1);assert(!anyka_management_take(m,&c));anyka_management_complete(m,c.revision,0);}
static void destroy(struct anyka_management *m)
{if(m->preview)preview_destroy(m->preview);close(m->directory);pthread_mutex_destroy(&m->lock);free(m);}
int main(void)
{
 char dir[]="/tmp/ptz-management-test-XXXXXX";assert(mkdtemp(dir));int fd=open(dir,O_RDONLY|O_DIRECTORY);assert(fd>=0);
 struct anyka_management_options o={.version="0.2.0-test",.boot_mode="ram_test",.initial_credentials="admin:camera123",.capabilities=3,.width=1920,.height=1080,.fps=15,.delegate=delegate};
 unsigned char legacy[48]={0};memcpy(legacy,"AKPIC001",8);put32(legacy+32,50);put32(legacy+40,1);put32(legacy+44,100);
 int legacyfd=openat(fd,"anyka-picture.conf",O_WRONLY|O_CREAT|O_EXCL,0600);assert(legacyfd>=0&&write(legacyfd,legacy,sizeof legacy)==sizeof legacy);close(legacyfd);
 struct anyka_management *m=NULL;assert(!anyka_management_open(&m,fd,&o));assert(!m->desired.encoder_configured&&!m->desired.advanced.configured&&m->desired.advanced.max_exposure_us==60000);
 struct control_reply r=call(m,"POST","/api/picture","hue=5");assert(r.status==409);finish(m);
 r=call(m,"POST","/api/picture","hue=5&saturation_offset=-7&contrast_offset=9&sharpness_offset=2&wdr_offset=8&mains_hz=60");assert(r.status==200);assert(m->desired.effects[0]==5&&m->desired.effects[2]==-7&&m->desired.mains_hz==60);
 struct anyka_management_command c;assert(anyka_management_take(m,&c)==1);assert(c.settings.effects[0]==5);anyka_management_complete(m,c.revision+1,-EIO);assert(pending(m));anyka_management_complete(m,c.revision,-EIO);assert(!pending(m)&&m->last_error==-EIO);
 r=call(m,"POST","/api/picture","hue=51");assert(r.status==400);r=call(m,"POST","/api/picture","hue=1&hue=2");assert(r.status==400);r=call(m,"POST","/api/picture","encoder_fps=60");assert(r.status==501);
 r=call(m,"POST","/api/settings","brightness=-2147483647");assert(r.status==400);r=call(m,"POST","/api/settings","shutter_ms=60");assert(r.status==501);r=call(m,"POST","/api/settings","microphone=false");assert(r.status==501);
 r=call(m,"POST","/api/picture","auto_exposure=0");assert(r.status==501);
 m->options.capabilities|=ANYKA_MANAGEMENT_ADVANCED;o.capabilities|=ANYKA_MANAGEMENT_ADVANCED;
 r=call(m,"POST","/api/picture","auto_exposure=0&exposure_us=20000&gamma_lift=50&temporal_nr=0");assert(r.status==200);finish(m);assert(m->desired.advanced.configured&&m->desired.advanced.exposure_us==20000);
 r=call(m,"POST","/api/settings","shutter_ms=60");assert(r.status==200);finish(m);assert(m->desired.advanced.max_exposure_us==60000);
 r=call(m,"POST","/api/picture","max_exposure_us=33333");assert(r.status==200);finish(m);
 m->options.capabilities|=ANYKA_MANAGEMENT_AUDIO;
 r=call(m,"POST","/api/settings","microphone=false&mic_gain=75");assert(r.status==200);finish(m);assert(m->desired.advanced.max_exposure_us==33333);
 r=call(m,"GET","/api/status",NULL);assert(strstr(r.json,"\"shutter_ms\":33.333"));
 r=call(m,"POST","/api/picture","encoder_gop=60");assert(r.status==501);
 m->options.capabilities|=ANYKA_MANAGEMENT_ENCODER;o.capabilities|=ANYKA_MANAGEMENT_ENCODER;
 r=call(m,"POST","/api/picture","encoder_gop=60");assert(r.status==501);
 struct anyka_encoder_snapshot enc={.settings={512,1024,30,35,25,45,1}};
 anyka_management_encoder_state(m,&enc,1);
 r=call(m,"POST","/api/picture/keyframe","");assert(r.status==202&&keyframes==1);
 r=call(m,"POST","/api/picture","encoder_target_kbps=2048&encoder_maximum_kbps=4096&encoder_gop=60&encoder_qp=30&encoder_min_qp=20&encoder_max_qp=40&encoder_rate_control=0");assert(r.status==200);finish(m);
 assert(m->desired.encoder_configured&&m->desired.encoder.gop_frames==60&&m->desired.encoder.target_kbps==2048);
 for(unsigned i=0;i<5;i++){const char *bad[]={"encoder_gop=1001","encoder_qp=19","encoder_target_kbps=5000","encoder_rate_control=2","encoder_min_qp=-1"};r=call(m,"POST","/api/picture",bad[i]);assert(r.status==400);}
 struct stat state;assert(!fstatat(fd,"anyka-picture.conf",&state,0)&&state.st_size==136);
 r=call(m,"POST","/api/rotation","rotate_180=true");assert(r.status==200);finish(m);
 r=call(m,"POST","/api/display","camera_name=Living+room&timezone=UTC&timestamp_enabled=false");assert(r.status==200);finish(m);assert(!strcmp(m->display.name,"Living room"));
 r=call(m,"POST","/api/display","timestamp_enabled=true");assert(r.status==501);
 r=call(m,"POST","/api/credentials","service=web&username=owner&password=simple123&confirm=simple123");assert(r.status==200);assert(camera_credentials_verify(0,"owner","simple123"));assert(!camera_credentials_verify(1,"owner","simple123"));
 r=call(m,"GET","/api/wifi",NULL);assert(r.status==200&&delegated==1);
 pthread_mutex_lock(&m->lock);assert(!anyka_management_take(m,&c));pthread_mutex_unlock(&m->lock);
 destroy(m);assert(!anyka_management_open(&m,fd,&o));assert(m->desired.effects[0]==5&&m->desired.rotate_180&&m->display.timezone==1&&m->desired.advanced.exposure_us==20000&&m->desired.advanced.gamma_lift==50);assert(camera_credentials_verify(0,"owner","simple123"));assert(m->desired.encoder_configured&&m->desired.encoder.gop_frames==60);finish(m);
 unsigned char v2[136];int old=openat(fd,"anyka-picture.conf",O_RDONLY);assert(old>=0&&read(old,v2,sizeof v2)==sizeof v2);close(old);memcpy(v2,"AKPIC002",8);put32(v2+56,60000);put32(v2+64,60000);old=openat(fd,"anyka-picture.conf",O_WRONLY|O_TRUNC);assert(old>=0&&write(old,v2,104)==104);close(old);destroy(m);m=NULL;assert(!anyka_management_open(&m,fd,&o));assert(!m->desired.encoder_configured&&m->desired.encoder.target_kbps==512);finish(m);
 destroy(m);m=NULL;o.fps=25;assert(!anyka_management_open(&m,fd,&o));assert(m->desired.advanced.max_exposure_us==39964&&m->desired.advanced.exposure_us==39964);finish(m);
 r=call(m,"POST","/api/settings","shutter_ms=60");assert(r.status==400);
 r=call(m,"POST","/api/picture","exposure_us=40000");assert(r.status==400);
 r=call(m,"POST","/api/picture","max_exposure_us=39000&exposure_us=39000");assert(r.status==200);finish(m);
 int corrupt=openat(fd,"anyka-picture.conf",O_WRONLY|O_TRUNC);assert(corrupt>=0);assert(write(corrupt,"x",1)==1);close(corrupt);destroy(m);m=NULL;assert(anyka_management_open(&m,fd,&o)==-EPROTO&&!m);
 unlinkat(fd,"anyka-picture.conf",0);unlinkat(fd,"display.conf",0);unlinkat(fd,"credentials.conf",0);close(fd);assert(!rmdir(dir));
 puts("PASS management: atomic persistence/reload, serialized queue/stale completion, validation, truthful unsupported controls, display, separate credentials and delegation");
}
