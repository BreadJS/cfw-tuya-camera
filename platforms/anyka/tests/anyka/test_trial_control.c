#include "../../src/anyka/trial_control.c"
#include <assert.h>
static struct control_http_config saved;
static int listen_error;
int control_http_start(const struct control_http_config *config)
{saved=*config;return listen_error;}
int control_json_string(char *out,size_t cap,const char *text)
{
 size_t at=0;if(cap<3)return -1;out[at++]='"';
 for(;*text;text++){if(*text=='"'||*text=='\\'){if(at+2>=cap)return -1;out[at++]='\\';}if(at+1>=cap)return -1;out[at++]=*text;}
 if(at+2>cap)return -1;
 out[at++]='"';out[at]=0;return 0;
}
int main(void)
{
 const uint8_t sps[]={0x67,77,0,40},pps[]={0x68,0x80},idr[]={0,0,1,0x65,0x80};
 struct anyka_trial_control_config config={"127.0.0.1","admin:camera123","Trial \"camera\"","experimental",81,1920,1080,15,{sps,sizeof sps,pps,sizeof pps,CAMERA_AUDIO_L16,16000}};
 struct anyka_trial_control *c=NULL;
 assert(anyka_trial_control_start(NULL,&config)==-EINVAL);
 listen_error=-EADDRINUSE;assert(anyka_trial_control_start(&c,&config)==-EADDRINUSE&&!c);
 listen_error=0;assert(!anyka_trial_control_start(&c,&config)&&c);
 assert(!strcmp(saved.credentials,"admin:camera123"));
 struct control_request req={.method="GET",.path="/api/status"};struct control_reply reply;
 saved.backend(saved.opaque,&req,&reply);assert(reply.status==200&&strstr(reply.json,"\"settings\":false")&&strstr(reply.json,"Trial \\\"camera\\\""));
 struct camera_media_frame f={idr,sizeof idr,1000000,1,0,0};
 assert(!anyka_trial_control_video(c,&f));anyka_trial_control_audio(c,&f);
 saved.backend(saved.opaque,&req,&reply);assert(strstr(reply.json,"\"frames\":1")&&strstr(reply.json,"\"blocks\":1"));
 req.path="/api/preview/status";saved.backend(c,&req,&reply);assert(reply.status==200&&strstr(reply.json,"avc1.4D0028"));
 req.path="/api/update";req.method="POST";saved.backend(c,&req,&reply);assert(reply.status==405);
 req.method="GET";saved.backend(c,&req,&reply);assert(reply.status==404);
 anyka_trial_control_error(c,-EIO);anyka_trial_control_error(c,-EAGAIN);
 req.path="/api/status";saved.backend(c,&req,&reply);assert(strstr(reply.json,"\"last_error\":-5")&&strstr(reply.json,"\"running\":false"));
 assert(!anyka_trial_control_video(NULL,&f));preview_destroy(c->preview);free(c);
 puts("PASS trial control: read-only routes, copied credentials, snapshot counters, preview MIME, sticky errors, startup rollback");
}
