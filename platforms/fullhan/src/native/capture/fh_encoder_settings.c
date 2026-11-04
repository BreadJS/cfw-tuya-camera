#include "fh_encoder_settings.h"
#include <errno.h>
#include <string.h>
static uint32_t get32(const uint8_t *p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
void fh_encoder_settings_defaults(struct fh_encoder_settings *s){if(s)*s=(struct fh_encoder_settings){1920,1080,15,30,77,28,28,28,FH_ENCODER_RATE_FIXED_QP,0};}
int fh_encoder_settings_validate(const struct fh_encoder_settings *s){
 if(!s)return -EINVAL;
 if(s->rate_mode!=FH_ENCODER_RATE_FIXED_QP||s->bitrate_kbps)return -ENOTSUP;
 if(!((s->width==1920&&s->height==1080&&s->fps>=1&&s->fps<=15)||(s->width==1280&&s->height==720&&s->fps==30)))return -ERANGE;
 if(!s->gop||s->gop>65535||(s->profile!=66&&s->profile!=77)||s->qp>51||s->i_qp>51||s->p_qp>51)return -ERANGE;
 return 0;
}
int fh_encoder_settings_plan(const struct fh_encoder_settings *s,const struct fh_encoder_settings *active,struct fh_encoder_plan *out){
 if(!out)return -EINVAL;
 int rc=fh_encoder_settings_validate(s);if(rc)return rc;
 if(active){rc=fh_encoder_settings_validate(active);if(rc)return rc;}
 unsigned changed=FH_ENCODER_ALL;
 if(active){changed=0;if(s->width!=active->width||s->height!=active->height)changed|=FH_ENCODER_GEOMETRY;if(s->fps!=active->fps)changed|=FH_ENCODER_FPS;if(s->gop!=active->gop)changed|=FH_ENCODER_GOP;if(s->profile!=active->profile)changed|=FH_ENCODER_PROFILE;if(s->qp!=active->qp||s->i_qp!=active->i_qp||s->p_qp!=active->p_qp)changed|=FH_ENCODER_QUANTIZERS;if(s->rate_mode!=active->rate_mode||s->bitrate_kbps!=active->bitrate_kbps)changed|=FH_ENCODER_RATE;}
 struct fh_encoder_plan p={.capture={.channel=0,.fps=s->fps,.gop=s->gop,.profile=s->profile,.qp=s->qp,.i_qp=s->i_qp,.p_qp=s->p_qp,.experimental_fps30=s->fps==30,.output_width=s->width,.output_height=s->height},.changed=changed,.restart_required=!!changed};
 *out=p;return 0;
}
int fh_encoder_settings_encode(const struct fh_encoder_settings *s,uint8_t *out,size_t capacity){
 if(!out||capacity<FH_ENCODER_SETTINGS_BYTES)return -EINVAL;
 int rc=fh_encoder_settings_validate(s);if(rc)return rc;
 uint8_t b[FH_ENCODER_SETTINGS_BYTES]={0};memcpy(b,"NENC",4);put32(b+4,1);put32(b+8,sizeof b);
 uint32_t words[]={s->width,s->height,s->fps,s->gop,s->profile,s->qp,s->i_qp,s->p_qp,s->rate_mode,s->bitrate_kbps};
 for(unsigned i=0;i<10;i++)put32(b+12+4*i,words[i]);
 put32(b+60,crc32(b,60));memcpy(out,b,sizeof b);return 0;
}
int fh_encoder_settings_decode(const uint8_t *b,size_t bytes,struct fh_encoder_settings *out){
 if(!b||!out)return -EINVAL;
 if(bytes!=FH_ENCODER_SETTINGS_BYTES||memcmp(b,"NENC",4)||get32(b+4)!=1||get32(b+8)!=FH_ENCODER_SETTINGS_BYTES||get32(b+52)||get32(b+56)||get32(b+60)!=crc32(b,60))return -EBADMSG;
 struct fh_encoder_settings s={get32(b+12),get32(b+16),get32(b+20),get32(b+24),get32(b+28),get32(b+32),get32(b+36),get32(b+40),get32(b+44),get32(b+48)};
 int rc=fh_encoder_settings_validate(&s);if(rc)return rc;*out=s;return 0;
}
const char *fh_encoder_bitrate_unsupported_reason(void){return "CBR/VBR mode mapping and complete rate-controller semantics are not verified; fixed QP only";}
const char *fh_encoder_live_unsupported_reason(void){return "Encoder changes require restart: live rate-controller synchronization and SPS/PPS continuity are not verified";}
