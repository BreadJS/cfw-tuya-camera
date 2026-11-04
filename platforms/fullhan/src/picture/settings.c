#define _GNU_SOURCE
#include "settings.h"
#include "../native/control/http.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
_Static_assert(sizeof(unsigned)==4&&sizeof(int)==4,"Canonical picture fields require32-bit integers");
struct field {const char *name;size_t offset;unsigned is_signed;};
#define U(name,member) {name,offsetof(struct camera_picture_settings,member),0}
#define I(name,member) {name,offsetof(struct camera_picture_settings,member),1}
static const struct field fields[]={
 U("saturation",isp.saturation),U("contrast",isp.contrast),I("luma_offset",isp.luma_offset),I("hue_degrees",isp.hue_degrees),
 U("cb_gain",isp.cb_gain),U("cr_gain",isp.cr_gain),U("temporal_nr",isp.temporal_nr),U("detail_percent",isp.detail_percent),
 U("auto_exposure",auto_exposure),U("exposure_us",exposure_us),U("gain_q6",gain_q6),U("min_gain_q6",min_gain_q6),U("max_gain_q6",max_gain_q6),
 U("auto_white_balance",auto_white_balance),U("wb_red_q9",wb_red_q9),U("wb_green_q9",wb_green_q9),U("wb_blue_q9",wb_blue_q9),
 U("gamma_enabled",gamma_enabled),U("gamma_profile",gamma_profile),
 U("shadow_enabled",shadow_enabled),U("shadow_target",shadow_target),U("shadow_bias",shadow_bias),U("shadow_low",shadow_low),U("shadow_middle",shadow_middle),U("shadow_high",shadow_high),
 U("ccm_mode",ccm_mode),I("ccm_0",ccm[0]),I("ccm_1",ccm[1]),I("ccm_2",ccm[2]),I("ccm_3",ccm[3]),I("ccm_4",ccm[4]),I("ccm_5",ccm[5]),
 I("ccm_6",ccm[6]),I("ccm_7",ccm[7]),I("ccm_8",ccm[8]),I("ccm_9",ccm[9]),I("ccm_10",ccm[10]),I("ccm_11",ccm[11]),
 U("encoder_width",encoder.width),U("encoder_height",encoder.height),U("encoder_fps",encoder.fps),U("encoder_gop",encoder.gop),U("encoder_profile",encoder.profile),
 U("encoder_qp",encoder.qp),U("encoder_i_qp",encoder.i_qp),U("encoder_p_qp",encoder.p_qp),U("encoder_rate_mode",encoder.rate_mode),U("encoder_bitrate_kbps",encoder.bitrate_kbps)
};
#undef U
#undef I
_Static_assert(sizeof fields/sizeof *fields==CAMERA_PICTURE_FORM_FIELDS,"Picture form field inventory");
#define ROOT_WORDS 38u
#define ENCODER_OFFSET (8u+ROOT_WORDS*4u)
_Static_assert(ENCODER_OFFSET+FH_ENCODER_SETTINGS_BYTES==224,"Canonical picture encoder offset");
static atomic_uint serial;
static int directory(int fd){struct stat s;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-EPERM:0;}
static int private_file(const struct stat *s){return !S_ISREG(s->st_mode)||s->st_uid!=geteuid()||(s->st_mode&07777)!=0600||s->st_nlink!=1;}
static uint32_t get32(const uint8_t *p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static uint32_t field_get(const struct camera_picture_settings *s,const struct field *f)
{if(f->is_signed){int v;memcpy(&v,(const char *)s+f->offset,4);return(uint32_t)v;}unsigned v;memcpy(&v,(const char *)s+f->offset,4);return v;}
static int signed32(uint32_t v){return v<=INT_MAX?(int)v:-1-(int)(UINT32_MAX-v);}
static void field_put(struct camera_picture_settings *s,const struct field *f,uint32_t v)
{if(f->is_signed){int x=signed32(v);memcpy((char *)s+f->offset,&x,4);}else memcpy((char *)s+f->offset,&v,4);}
void camera_picture_defaults(struct camera_picture_settings *s)
{
 if(!s)return;
 memset(s,0,sizeof *s);fh_picture_isp_defaults(&s->isp);s->auto_exposure=1;s->exposure_us=10000;s->gain_q6=s->min_gain_q6=64;s->max_gain_q6=1024;
 s->auto_white_balance=1;s->wb_red_q9=s->wb_green_q9=s->wb_blue_q9=512;
 s->gamma_enabled=1;s->gamma_profile=5;s->shadow_enabled=1;s->shadow_target=106;s->shadow_bias=10;s->shadow_low=s->shadow_middle=64;s->shadow_high=255;
 s->ccm[0]=s->ccm[4]=s->ccm[8]=512;fh_encoder_settings_defaults(&s->encoder);
}
int camera_picture_valid(const struct camera_picture_settings *s)
{
 if(!s||!fh_picture_isp_valid(&s->isp)||fh_encoder_settings_validate(&s->encoder))return 0;
 if(s->auto_exposure>1||s->exposure_us<100||s->exposure_us>60000||s->gain_q6<64||s->gain_q6>1024||
  s->min_gain_q6<64||s->max_gain_q6>1024||s->min_gain_q6>s->max_gain_q6)return 0;
 if(!s->auto_exposure&&s->encoder.fps==30&&s->exposure_us>33000)return 0;
 if(s->auto_white_balance>1||s->wb_red_q9<256||s->wb_red_q9>2048||s->wb_green_q9<256||s->wb_green_q9>2048||s->wb_blue_q9<256||s->wb_blue_q9>2048)return 0;
 if(s->gamma_enabled>1||s->gamma_profile>9||s->shadow_enabled>1||!s->shadow_target||s->shadow_target>255||s->shadow_bias>15||
  s->shadow_low<16||s->shadow_low>128||s->shadow_middle<16||s->shadow_middle>128||s->shadow_high<16||s->shadow_high>255||s->ccm_mode>2)return 0;
 for(unsigned i=0;i<12;i++)if(s->ccm[i]<-4096||s->ccm[i]>4095)return 0;
 return 1;
}
static int encode(const struct camera_picture_settings *s,uint8_t b[CAMERA_PICTURE_BYTES])
{
 if(!camera_picture_valid(s))return -EINVAL;
 memset(b,0,CAMERA_PICTURE_BYTES);memcpy(b,"CAMPICT1",8);
 for(unsigned i=0;i<ROOT_WORDS;i++)put32(b+8+4*i,field_get(s,&fields[i]));
 int rc=fh_encoder_settings_encode(&s->encoder,b+ENCODER_OFFSET,FH_ENCODER_SETTINGS_BYTES);if(rc)return rc;
 put32(b+252,crc32(b,252));return 0;
}
static int decode(const uint8_t b[CAMERA_PICTURE_BYTES],struct camera_picture_settings *out)
{
 if(memcmp(b,"CAMPICT1",8)||get32(b+252)!=crc32(b,252))return -EBADMSG;
 for(unsigned i=224;i<252;i++)if(b[i])return -EBADMSG;
 struct camera_picture_settings s;memset(&s,0,sizeof s);
 for(unsigned i=0;i<ROOT_WORDS;i++)field_put(&s,&fields[i],get32(b+8+4*i));
 int rc=fh_encoder_settings_decode(b+ENCODER_OFFSET,FH_ENCODER_SETTINGS_BYTES,&s.encoder);if(rc)return rc;
 if(!camera_picture_valid(&s))return -ERANGE;
 *out=s;return 0;
}
int camera_picture_load(int dir,struct camera_picture_settings *out)
{
 if(!out)return -EINVAL;
 int rc=directory(dir);if(rc)return rc;
 int fd=openat(dir,"picture.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){if(errno!=ENOENT)return -errno;camera_picture_defaults(out);return 0;}
 uint8_t b[CAMERA_PICTURE_BYTES];struct stat before,after;rc=-EPERM;
 if(fstat(fd,&before)||private_file(&before)||before.st_size!=sizeof b)goto done;
 size_t at=0;while(at<sizeof b){ssize_t n=read(fd,b+at,sizeof b-at);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;goto done;}at+=(size_t)n;}
 if(fstat(fd,&after)||private_file(&after)||after.st_dev!=before.st_dev||after.st_ino!=before.st_ino||after.st_size!=before.st_size||
  after.st_mtim.tv_sec!=before.st_mtim.tv_sec||after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec||
  after.st_ctim.tv_sec!=before.st_ctim.tv_sec||after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec)goto done;
 struct camera_picture_settings s;rc=decode(b,&s);if(rc)goto done;
 if(close(fd))return -errno;
 *out=s;return 0;
done:close(fd);return rc;
}
int camera_picture_save(int dir,const struct camera_picture_settings *s)
{
 if(!s)return -EINVAL;
 int rc=directory(dir);if(rc)return rc;
 uint8_t b[CAMERA_PICTURE_BYTES];rc=encode(s,b);if(rc)return rc;
 struct stat old;if(!fstatat(dir,"picture.conf",&old,AT_SYMLINK_NOFOLLOW)){if(private_file(&old))return -EPERM;}else if(errno!=ENOENT)return -errno;
 char name[80];int fd=-1;
 for(unsigned i=0;i<100;i++){
  unsigned next=atomic_fetch_add(&serial,1)+1;snprintf(name,sizeof name,"picture.tmp.%ld.%u",(long)getpid(),next);
  fd=openat(dir,name,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd>=0)break;if(errno!=EEXIST)return -errno;
 }
 if(fd<0)return -EEXIST;
 rc=-EIO;size_t at=0;if(fchmod(fd,0600))goto done;
 while(at<sizeof b){ssize_t n=write(fd,b+at,sizeof b-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;}
 if(fsync(fd))goto done;
 if(close(fd)){fd=-1;goto done;}fd=-1;
 if(renameat(dir,name,dir,"picture.conf")||fsync(dir))goto done;
 rc=0;
done:if(fd>=0)close(fd);if(rc)unlinkat(dir,name,0);return rc;
}
static int numeric(const char *text,unsigned is_signed,uint32_t *out)
{
 if(!*text)return -EINVAL;
 int negative=*text=='-';if(negative){if(!is_signed)return -EINVAL;text++;if(!*text)return -EINVAL;}
 uint64_t max=is_signed?(negative?UINT64_C(2147483648):INT_MAX):UINT32_MAX,value=0;
 for(;*text;text++){if(*text<'0'||*text>'9')return -EINVAL;unsigned digit=(unsigned)(*text-'0');if(value>(max-digit)/10)return -ERANGE;value=value*10+digit;}
 *out=negative?0u-(uint32_t)value:(uint32_t)value;return 0;
}
int camera_picture_parse_form(const char *body,struct camera_picture_settings *out)
{
 if(!body||!out)return -EINVAL;
 if(strlen(body)>4096)return -E2BIG;
 unsigned count=1;for(const char *p=body;*p;p++)if(*p=='&')count++;
 if(count!=CAMERA_PICTURE_FORM_FIELDS)return -EINVAL;
 struct camera_picture_settings s;memset(&s,0,sizeof s);
 for(unsigned i=0;i<CAMERA_PICTURE_FORM_FIELDS;i++){
  char text[32];if(control_form(body,fields[i].name,text,sizeof text)!=1)return -EINVAL;
  uint32_t value;int rc=numeric(text,fields[i].is_signed,&value);if(rc)return rc;field_put(&s,&fields[i],value);
 }
 int rc=fh_encoder_settings_validate(&s.encoder);if(rc)return rc;
 if(!camera_picture_valid(&s))return -ERANGE;
 *out=s;return 0;
}
int camera_picture_json(const struct camera_picture_settings *s,char *out,size_t capacity)
{
 if(!s||!out||!camera_picture_valid(s))return -EINVAL;
 char json[4096];size_t used=0;json[used++]='{';
 for(unsigned i=0;i<CAMERA_PICTURE_FORM_FIELDS;i++){
  uint32_t v=field_get(s,&fields[i]);int n;
  if(fields[i].is_signed)n=snprintf(json+used,sizeof json-used,"%s\"%s\":%d",i?",":"",fields[i].name,signed32(v));
  else n=snprintf(json+used,sizeof json-used,"%s\"%s\":%u",i?",":"",fields[i].name,v);
  if(n<0||(size_t)n>=sizeof json-used)return -ENOSPC;
  used+=(size_t)n;
 }
 json[used++]='}';json[used]=0;if(capacity<=used)return -ENOSPC;
 memcpy(out,json,used+1);return 0;
}
