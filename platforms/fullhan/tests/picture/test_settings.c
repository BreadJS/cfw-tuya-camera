#define _GNU_SOURCE
#include "../../src/picture/settings.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Publisher is never used by these pure persistence/form tests. */
int fh_linux_io_isp_read(void *p,uint32_t o,uint32_t *v){(void)p;(void)o;(void)v;return -ENOTSUP;}
int fh_linux_io_isp_write(void *p,uint32_t o,uint32_t v){(void)p;(void)o;(void)v;return -ENOTSUP;}
static uint32_t get32(const uint8_t *p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static void form(const struct camera_picture_settings *s,char *out)
{
 char json[4096];assert(!camera_picture_json(s,json,sizeof json));size_t n=0;
 for(const char *p=json;*p;p++){if(*p=='{'||*p=='}'||*p=='"')continue;out[n++]=*p==':'?'=':*p==','?'&':*p;}out[n]=0;
}
static void replace(const char *original,const char *key,const char *value,char *out)
{
 const char *at=original;size_t keylen=strlen(key);
 while((at=strstr(at,key))){if((at==original||at[-1]=='&')&&at[keylen]=='=')break;at++;}assert(at);
 size_t prefix=(size_t)(at-original)+keylen+1;const char *tail=strchr(original+prefix,'&');if(!tail)tail=original+strlen(original);
 memcpy(out,original,prefix);strcpy(out+prefix,value);strcpy(out+prefix+strlen(value),tail);
}
static void rejected(const char *body)
{
 struct camera_picture_settings out,old;memset(&out,0x5a,sizeof out);old=out;
 assert(camera_picture_parse_form(body,&out)<0&&!memcmp(&out,&old,sizeof out));
}
static void forms(void)
{
 struct camera_picture_settings s,out;camera_picture_defaults(&s);assert(camera_picture_valid(&s));
 char body[4096],bad[4096],json[4096];form(&s,body);assert(!camera_picture_parse_form(body,&out)&&!memcmp(&s,&out,sizeof s));
 unsigned fields=1;for(char *p=body;*p;p++)fields+=*p=='&';assert(fields==48);
 assert(!camera_picture_json(&s,json,sizeof json)&&strstr(json,"\"shadow_target\":106")&&strstr(json,"\"encoder_rate_mode\":2"));
 strcpy(json,"unchanged");assert(camera_picture_json(&s,json,1)==-ENOSPC&&!strcmp(json,"unchanged"));
 const char *keys[]={"saturation","contrast","luma_offset","hue_degrees","cb_gain","cr_gain","temporal_nr","detail_percent","auto_exposure","exposure_us","gain_q6","min_gain_q6","max_gain_q6","auto_white_balance","wb_red_q9","wb_green_q9","wb_blue_q9","gamma_enabled","gamma_profile","shadow_enabled","shadow_target","shadow_bias","shadow_low","shadow_middle","shadow_high","ccm_mode","ccm_0","encoder_width","encoder_height","encoder_fps","encoder_gop","encoder_profile","encoder_qp","encoder_i_qp","encoder_p_qp","encoder_rate_mode","encoder_bitrate_kbps"};
 const char *values[]={"1021","256","-129","181","201","201","2","201","true","60001","63","1025","63","2","255","2049","255","2","10","2","0","16","15","129","256","3","-4097","640","480","30","0","100","52","52","52","1","1024"};
 assert(sizeof keys/sizeof *keys==sizeof values/sizeof *values);
 for(unsigned i=0;i<sizeof keys/sizeof *keys;i++){replace(body,keys[i],values[i],bad);rejected(bad);}
 const char *syntax[]={"","+1"," 1","-0","4294967296","999999999999999999999999999999999","1e2","%","%GG"};
 for(unsigned i=0;i<sizeof syntax/sizeof *syntax;i++){replace(body,"auto_exposure",syntax[i],bad);rejected(bad);}
 assert(strlen(body)+9<sizeof bad);strcpy(bad,body);strcat(bad,"&extra=1");rejected(bad);rejected(strchr(body,'&')+1);
 strcpy(bad,body);char *name=strstr(bad,"contrast=");assert(name);memcpy(name,"unknownX",8);rejected(bad);
 /* Duplicate one known key while preserving the exact field count. */
 name=strstr(body,"cb_gain=");assert(name);size_t n=(size_t)(name-body);memcpy(bad,body,n);strcpy(bad+n,"cr_gain");strcpy(bad+n+7,name+7);rejected(bad);
 s.auto_exposure=0;s.exposure_us=33000;s.gain_q6=1024;s.auto_white_balance=0;s.wb_red_q9=2048;s.wb_green_q9=256;s.wb_blue_q9=700;
 s.isp.luma_offset=-128;s.isp.hue_degrees=-180;s.gamma_enabled=0;s.gamma_profile=9;s.shadow_target=255;s.shadow_bias=15;s.shadow_low=16;s.shadow_middle=128;s.shadow_high=16;s.ccm_mode=2;
 for(unsigned i=0;i<12;i++)s.ccm[i]=i&1?4095:-4096;
 s.encoder=(struct fh_encoder_settings){1280,720,30,65535,66,0,51,14,2,0};assert(camera_picture_valid(&s));form(&s,body);assert(!camera_picture_parse_form(body,&out)&&!memcmp(&s,&out,sizeof s));
 s.exposure_us=33001;assert(!camera_picture_valid(&s));s.auto_exposure=1;assert(camera_picture_valid(&s));
 puts("Picture forms:48 exact fields, signed/full-boundary values, unsupported combinations and output preservation PASS");
}
static void write_file(int dir,const uint8_t *b,size_t size)
{int fd=openat(dir,"picture.conf",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);assert(fd>=0&&write(fd,b,size)==(ssize_t)size);assert(!fchmod(fd,0600));close(fd);}
static void failed_load(int dir)
{struct camera_picture_settings s,old;memset(&s,0x5a,sizeof s);old=s;assert(camera_picture_load(dir,&s)<0&&!memcmp(&s,&old,sizeof s));}
static void persistence(const char *path)
{
 int dir=open(path,O_RDONLY|O_DIRECTORY);assert(dir>=0);struct camera_picture_settings s,loaded;camera_picture_defaults(&s);
 assert(!camera_picture_load(dir,&loaded)&&!memcmp(&s,&loaded,sizeof s));assert(faccessat(dir,"picture.conf",F_OK,0)&&errno==ENOENT);
 assert(!camera_picture_save(dir,&s)&&!camera_picture_load(dir,&loaded)&&!memcmp(&s,&loaded,sizeof s));
 struct stat before,after;assert(!fstatat(dir,"picture.conf",&before,0)&&(before.st_mode&07777)==0600);
 uint8_t b[257]={0};int fd=openat(dir,"picture.conf",O_RDONLY);assert(fd>=0&&read(fd,b,256)==256);
 assert(!memcmp(b,"CAMPICT1",8)&&get32(b+8)==320&&get32(b+88)==106&&get32(b+112)==512&&get32(b+144)==512&&!memcmp(b+160,"NENC",4));
 for(unsigned i=224;i<252;i++){assert(!b[i]);}assert(get32(b+252)==crc32(b,252));
 s.isp.saturation=123;s.isp.luma_offset=-25;s.ccm[9]=-1234;assert(!camera_picture_save(dir,&s));assert(!fstatat(dir,"picture.conf",&after,0)&&before.st_ino!=after.st_ino);
 uint8_t old[256];assert(pread(fd,old,sizeof old,0)==256&&!memcmp(b,old,sizeof old));close(fd);assert(!camera_picture_load(dir,&loaded)&&!memcmp(&s,&loaded,sizeof s));
 for(unsigned which=0;which<4;which++){
  uint8_t corrupt[256];memcpy(corrupt,b,sizeof corrupt);corrupt[which==0?0:which==1?70:which==2?224:8+8*4]^=which==3?3:1;
  if(which>=2){put32(corrupt+252,crc32(corrupt,252));}write_file(dir,corrupt,sizeof corrupt);failed_load(dir);
 }
 write_file(dir,b,255);failed_load(dir);write_file(dir,b,257);failed_load(dir);
 write_file(dir,b,256);assert(!fchmodat(dir,"picture.conf",0644,0));failed_load(dir);assert(camera_picture_save(dir,&s)<0);
 assert(!fchmodat(dir,"picture.conf",0600,0));assert(!linkat(dir,"picture.conf",dir,"second",0));failed_load(dir);assert(camera_picture_save(dir,&s)<0);assert(!unlinkat(dir,"second",0));
 assert(!unlinkat(dir,"picture.conf",0));assert(!symlinkat("outside",dir,"picture.conf"));failed_load(dir);assert(camera_picture_save(dir,&s)<0);assert(!unlinkat(dir,"picture.conf",0));
 assert(!camera_picture_save(dir,&s));struct camera_picture_settings invalid=s;invalid.shadow_target=0;assert(camera_picture_save(dir,&invalid)<0);assert(!camera_picture_load(dir,&loaded)&&!memcmp(&s,&loaded,sizeof s));
 assert(!fchmod(dir,0755));failed_load(dir);assert(camera_picture_save(dir,&s)<0);assert(!fchmod(dir,0700));close(dir);
 puts("Picture persistence: canonical256B CRC/encoder record, defaults, atomic replacement, corrupt/reserved/private/link checks PASS");
}
int main(int argc,char **argv){assert(argc==2);forms();persistence(argv[1]);return 0;}
