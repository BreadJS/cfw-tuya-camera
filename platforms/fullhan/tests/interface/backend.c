#define _GNU_SOURCE
#include "../../src/native/control/app.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct input {const char *p;size_t left;};
static ssize_t read_input(void *p,void *out,size_t n){struct input *i=p;if(n>i->left)n=i->left;memcpy(out,i->p,n);i->p+=n;i->left-=n;return (ssize_t)n;}
static struct control_reply request(struct control_app *a,const char *method,const char *body){struct input in={body,strlen(body)};struct control_request q={method,"/api/display","application/x-www-form-urlencoded",{&in,in.left,read_input}};struct control_reply r={0};control_app_backend(a,&q,&r);return r;}
static struct control_reply rotation(struct control_app *a,const char *method,const char *body){struct input in={body,strlen(body)};struct control_request q={method,"/api/rotation","application/x-www-form-urlencoded",{&in,in.left,read_input}};struct control_reply r={0};control_app_backend(a,&q,&r);return r;}
static struct control_reply picture(struct control_app *a,const char *path,const char *method,const char *body){struct input in={body,strlen(body)};struct control_request q={method,path,"application/x-www-form-urlencoded",{&in,in.left,read_input}};struct control_reply r={0};control_app_backend(a,&q,&r);return r;}
static void picture_form(const struct camera_picture_settings *s,char body[4096]){char json[4096];assert(!camera_picture_json(s,json,sizeof json));size_t at=0;for(const char *p=json;*p;p++){if(*p=='"'||*p=='{'||*p=='}')continue;body[at++]=*p==':'?'=':*p==','?'&':*p;}body[at]=0;}
static unsigned illumination_calls;
static int lighting(void *p,unsigned night,unsigned ir){(void)p;assert(!night&&!ir);illumination_calls++;return 0;}
static int lighting_status(void *p,char *out,size_t n){(void)p;snprintf(out,n,"{\"available\":true}");return 0;}
static unsigned enabled,calls;static int error;
static int apply(void *p,unsigned e,const char *text){(void)p;assert(strlen(text)==23);enabled=e;calls++;return error;}
int main(int argc,char **argv){
 assert(argc==2);char config[512],ram[512];snprintf(config,sizeof config,"%s/config",argv[1]);snprintf(ram,sizeof ram,"%s/ram",argv[1]);
 struct control_app_options options={config,ram,"nonexistent-key","nonexistent-wifi","nonexistent-socket","test:password",0,1920,1080,15};
 struct control_settings defaults={100,10,50,1,100};struct control_app *a=NULL;assert(!control_app_open(&a,&options,&defaults));
 {struct camera_picture_settings p,loaded;uint64_t generation;camera_picture_defaults(&p);
  p.isp.saturation=640;p.auto_exposure=0;p.exposure_us=20000;p.gain_q6=128;p.auto_white_balance=0;p.wb_red_q9=700;p.wb_blue_q9=1000;p.gamma_profile=3;p.shadow_bias=8;p.ccm_mode=2;
  p.encoder.fps=10;p.encoder.gop=20;p.encoder.profile=66;p.encoder.qp=24;p.encoder.i_qp=22;p.encoder.p_qp=26;
  char body[4096];picture_form(&p,body);struct control_reply reply=picture(a,"/api/picture","POST",body);
  assert(reply.status==200&&strstr(reply.json,"\"live_pending\":true")&&strstr(reply.json,"\"restart_required\":true"));
  assert(!control_app_picture_settings(a,&loaded,&generation)&&loaded.isp.saturation==640&&loaded.encoder.fps==10&&loaded.exposure_us==20000);
  control_app_picture_telemetry(a,generation,0);control_app_encoder_active(a,&loaded.encoder);reply=picture(a,"/api/picture","GET","");
  assert(strstr(reply.json,"\"live_pending\":false")&&strstr(reply.json,"\"restart_required\":false"));
  struct control_app *persisted=NULL;assert(!control_app_open(&persisted,&options,&defaults));assert(!control_app_picture_settings(persisted,&loaded,NULL)&&loaded.gamma_profile==3&&loaded.wb_red_q9==700);
  reply=picture(a,"/api/picture","POST","saturation=1");assert(reply.status==422);assert(!control_app_picture_settings(a,&loaded,NULL)&&loaded.isp.saturation==640);
  reply=picture(a,"/api/picture","POST","action=defaults");assert(reply.status==200);assert(!control_app_picture_settings(a,&loaded,&generation)&&loaded.isp.saturation==320&&loaded.auto_exposure==1&&loaded.encoder.fps==15);
  control_app_encoder_active(a,&loaded.encoder);control_app_picture_telemetry(a,generation,-EIO);reply=picture(a,"/api/picture","GET","");assert(strstr(reply.json,"\"last_error\":-5"));
  reply=picture(a,"/api/restart","POST","");assert(reply.status==400);
  reply=picture(a,"/api/restart","POST","confirm=restart");assert(reply.status==503); /* Host/RAM tests never reboot. */}
 {struct control_reply reply=rotation(a,"GET","");assert(reply.status==200&&strstr(reply.json,"\"requested_180\":false"));
  reply=rotation(a,"POST","rotate_180=true");assert(reply.status==200&&strstr(reply.json,"\"pending\":true"));
  unsigned requested;assert(!control_app_rotation_requested(a,&requested)&&requested==1);
  control_app_rotation_telemetry(a,1,0);reply=rotation(a,"GET","");assert(strstr(reply.json,"\"pending\":false")&&strstr(reply.json,"\"applied_180\":true"));
  struct control_app *persisted=NULL;assert(!control_app_open(&persisted,&options,&defaults));assert(!control_app_rotation_requested(persisted,&requested)&&requested==1);
  const char *bad[]={"rotate_180=2","rotate_180=yes","rotate_180=true&extra=1","rotate_180=true&rotate_180=false","rotation=true",""};
  for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){reply=rotation(a,"POST",bad[i]);assert(reply.status==400);}
  reply=rotation(a,"POST","rotate_180=false");assert(reply.status==200);control_app_rotation_telemetry(a,1,-EIO);
  reply=rotation(a,"GET","");assert(strstr(reply.json,"\"last_error\":-5")&&strstr(reply.json,"\"applied_known\":false"));
  control_app_rotation_telemetry(a,0,0);}
 {struct control_request q={"GET","/api/status","",{0}};struct control_reply reply={0};
  control_app_stream_state(a,1,0,0);control_app_backend(a,&q,&reply);
  assert(reply.status==200&&strstr(reply.json,"\"stream\":{\"running\":true,\"last_error\":0,\"source_error\":0}"));
  control_app_stream_state(a,0,-1,-90);control_app_backend(a,&q,&reply);
  assert(strstr(reply.json,"\"stream\":{\"running\":false,\"last_error\":-1,\"source_error\":-90}"));}
 struct control_illumination_ops lights={NULL,lighting,lighting_status};assert(!control_app_illumination(a,&lights)&&illumination_calls==1);
 struct control_reply r=request(a,"GET","");assert(r.status==200&&strstr(r.json,"\"camera_name\":\"Camera\"")&&strstr(r.json,"\"timestamp_enabled\":false"));
 r=request(a,"POST","camera_name=Hallway&timestamp_enabled=true&timezone=UTC");assert(r.status==503);
 r=request(a,"POST","camera_name=Hallway&timestamp_enabled=false&timezone=UTC");assert(r.status==200);
 struct control_display_ops ops={NULL,apply};assert(!control_app_display(a,&ops));assert(!enabled);
 r=request(a,"POST","camera_name=Living+room&timestamp_enabled=true&timezone=Europe%2FAmsterdam");assert(r.status==200&&enabled);
 unsigned before=calls;control_app_display_tick(a);assert(calls==before+1&&enabled);
 r=request(a,"POST","camera_name=%3Cscript%3E&timestamp_enabled=false&timezone=UTC");assert(r.status==200&&!enabled&&!strstr(r.json,"<script>"));
 before=calls;control_app_display_tick(a);assert(calls==before);
 const char *bad[]={"camera_name=&timestamp_enabled=false&timezone=UTC","camera_name=A&timestamp_enabled=yes&timezone=UTC","camera_name=A&timestamp_enabled=false&timezone=Unknown","camera_name=A&camera_name=B&timestamp_enabled=false","camera_name=A&timestamp_enabled=false&timezone=UTC&extra=1","camera_name=%FF&timestamp_enabled=false&timezone=UTC"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){r=request(a,"POST",bad[i]);assert(r.status>=400&&r.status<500);}
 error=-EIO;r=request(a,"POST","camera_name=Kitchen&timestamp_enabled=true&timezone=UTC");assert(r.status==503&&strstr(r.json,"settings_saved_but_timestamp_failed"));
 r=request(a,"GET","");assert(r.status==200&&strstr(r.json,"Kitchen")&&strstr(r.json,"\"timestamp_error\":-5"));
 struct control_app *b=NULL;assert(!control_app_open(&b,&options,&defaults));r=request(b,"GET","");assert(r.status==200&&strstr(r.json,"Kitchen")&&strstr(r.json,"\"timestamp_enabled\":true"));
 puts("display backend: persistence, validation, JSON escaping, unavailable/failing OSD and disabled-clock ticks PASS");return 0;
}
