#include "../../src/native/capture/fh_encoder_settings.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static void roundtrip(struct fh_encoder_settings *s){
 struct fh_encoder_plan plan;struct fh_encoder_settings got;uint8_t record[FH_ENCODER_SETTINGS_BYTES];
 assert(fh_encoder_settings_plan(s,NULL,&plan)==0&&plan.restart_required&&plan.changed==FH_ENCODER_ALL);
 assert(fh_capture_validate_config(&plan.capture)==0);
 assert(plan.capture.fps==s->fps&&plan.capture.gop==s->gop&&plan.capture.profile==s->profile&&plan.capture.qp==s->qp&&plan.capture.i_qp==s->i_qp&&plan.capture.p_qp==s->p_qp);
 assert(plan.capture.output_width==s->width&&plan.capture.output_height==s->height&&plan.capture.experimental_fps30==(s->fps==30));
 assert(fh_encoder_settings_encode(s,record,sizeof record)==0);
 assert(fh_encoder_settings_decode(record,sizeof record,&got)==0&&!memcmp(s,&got,sizeof got));
 assert(fh_encoder_settings_plan(s,&got,&plan)==0&&!plan.restart_required&&!plan.changed);
}
int main(void){
 struct fh_encoder_settings s,decoded;fh_encoder_settings_defaults(&s);roundtrip(&s);
 uint8_t record[FH_ENCODER_SETTINGS_BYTES],bad[FH_ENCODER_SETTINGS_BYTES];assert(!fh_encoder_settings_encode(&s,record,sizeof record));
 assert(!memcmp(record,"NENC\x01\0\0\0\x40\0\0\0\x80\x07\0\0\x38\x04\0\0",20));
 const uint8_t expected_crc[4]={0x80,0x3c,0xf9,0xe9};assert(!memcmp(record+60,expected_crc,4));
 struct fh_encoder_settings untouched=s;
 for(unsigned bit=0;bit<64*8;bit++){memcpy(bad,record,64);bad[bit/8]^=(uint8_t)(1u<<(bit&7));decoded=untouched;assert(fh_encoder_settings_decode(bad,64,&decoded)==-EBADMSG&&!memcmp(&decoded,&untouched,sizeof decoded));}
 for(unsigned n=0;n<64;n++)assert(fh_encoder_settings_decode(record,n,&decoded)==-EBADMSG);
 assert(fh_encoder_settings_decode(record,65,&decoded)==-EBADMSG);
 const unsigned qps[]={0,28,51},gops[]={1,30,65535};unsigned count=0;
 for(unsigned mode=0;mode<2;mode++)for(unsigned fps=1;fps<=(mode?1u:15u);fps++)for(unsigned profile=66;profile<=77;profile+=11)for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++)for(unsigned c=0;c<3;c++)for(unsigned g=0;g<3;g++){
  fh_encoder_settings_defaults(&s);s.width=mode?1280:1920;s.height=mode?720:1080;s.fps=mode?30:fps;s.profile=profile;s.qp=qps[a];s.i_qp=qps[b];s.p_qp=qps[c];s.gop=gops[g];roundtrip(&s);count++;
 }
 fh_encoder_settings_defaults(&s);struct fh_encoder_plan p;memset(&p,0x55,sizeof p);struct fh_encoder_plan before=p;
 s.fps=30;assert(fh_encoder_settings_plan(&s,NULL,&p)==-ERANGE&&!memcmp(&p,&before,sizeof p));
 s.width=1280;s.height=720;s.fps=15;assert(fh_encoder_settings_validate(&s)==-ERANGE);
 s.fps=30;s.qp=52;assert(fh_encoder_settings_validate(&s)==-ERANGE);s.qp=28;s.profile=100;assert(fh_encoder_settings_validate(&s)==-ERANGE);
 fh_encoder_settings_defaults(&s);s.rate_mode=0;assert(fh_encoder_settings_validate(&s)==-ENOTSUP);s.rate_mode=1;assert(fh_encoder_settings_validate(&s)==-ENOTSUP);s.rate_mode=2;s.bitrate_kbps=4096;assert(fh_encoder_settings_validate(&s)==-ENOTSUP);
 fh_encoder_settings_defaults(&s);struct fh_encoder_settings active=s;s.i_qp=20;assert(!fh_encoder_settings_plan(&s,&active,&p)&&p.changed==FH_ENCODER_QUANTIZERS&&p.restart_required);
 s=active;s.gop=60;assert(!fh_encoder_settings_plan(&s,&active,&p)&&p.changed==FH_ENCODER_GOP&&p.restart_required);
 assert(fh_encoder_bitrate_unsupported_reason()[0]&&fh_encoder_live_unsupported_reason()[0]);
 printf("Encoder settings:%u supported startup configurations roundtrip and match capture validation;512 single-bit corruptions/truncations rejected; restart/unsupported guards PASS\n",count);
}
