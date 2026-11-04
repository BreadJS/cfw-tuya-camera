/* Mock exact SDK without an encoder device, SDK host or camera. */
#include "../../src/anyka/encoder_control.c"
#include <assert.h>
#include <stdio.h>
static struct anyka_encode_param model;
static unsigned get_calls,set_calls,qp_calls,iframe_calls,idr_calls;
static int get_error,set_error,ignore_set,idr_error,invalid_iframe;
static struct anyka_encoder_iframe frame={26,40,40,150};
static const char *version_text="libmpi_venc V1.0.29";
static const char *version(void){return version_text;}
static int get(unsigned handle,struct anyka_encode_param *out)
{assert(handle==3);get_calls++;if(get_error)return get_error;*out=model;return 0;}
static int set(unsigned handle,const struct anyka_encode_param *in)
{
 assert(handle==3);set_calls++;if(set_error)return set_error;
 if(!ignore_set)model=*in;
 return 0;
}
static int qp(unsigned handle,int low,int high)
{(void)handle;(void)low;(void)high;qp_calls++;return 0;}
static int iframe_get(unsigned handle,int32_t *low,int32_t *high,int32_t *min_size,int32_t *max_size)
{
 assert(handle==3&&!*low&&!*high&&!*min_size&&!*max_size);
 /* Shipped inner getter writes16bits despite the public int* declaration. */
 uint16_t a=(uint16_t)frame.minimum_qp,b=(uint16_t)(invalid_iframe?99:frame.maximum_qp);
 memcpy(low,&a,2);memcpy(high,&b,2);*min_size=(int32_t)frame.minimum_kbytes;*max_size=(int32_t)frame.maximum_kbytes;return 0;
}
static int iframe_set(unsigned handle,int low,int high,int min_size,int max_size)
{
 assert(handle==3);iframe_calls++;
 frame=(struct anyka_encoder_iframe){low?(unsigned)low:26,high?(unsigned)high:40,
     min_size?(unsigned)min_size:40,max_size?(unsigned)max_size:150};
 if(frame.maximum_kbytes>180)frame.maximum_kbytes=180;
 return 0;
}
static int idr(unsigned handle){assert(handle==3);idr_calls++;return idr_error;}
static const struct encoder_sdk sdk={version,get,set,qp,iframe_get,iframe_set,idr};
static void reset(void)
{
 memset(&model,0,sizeof model);model.width=1920;model.height=1080;model.fps=15;model.gop_frames=30;
 model.target_kbps=512;model.maximum_kbps=1024;model.profile=0;model.rate_control=1;
 model.initial_qp=35;model.minimum_qp=25;model.maximum_qp=45;model.chroma_mode=1;model.encoder_level=50;
 model.reserved_1a=0xa55a;model.reserved_2e=0xb66b;model.reserved_3a=0xc77c;model.reserved_28=0x200000;
 model.jpeg_quality=2;model.smart_quality=64;model.smart_static_value=128;
 get_calls=set_calls=qp_calls=iframe_calls=idr_calls=0;get_error=set_error=ignore_set=idr_error=invalid_iframe=0;
 frame=(struct anyka_encoder_iframe){26,40,40,150};version_text="libmpi_venc V1.0.29";
}
int main(void)
{
 reset();struct anyka_encoder_control *c=NULL;version_text="libmpi_venc V1.0.30";
 assert(open_bound(&c,3,&sdk)==-EPROTO&&!c&&!get_calls);
 version_text=NULL;assert(open_bound(&c,3,&sdk)==-EPROTO&&!c&&!get_calls);
 version_text="libmpi_venc V1.0.29";assert(open_bound(&c,8,&sdk)==-EINVAL&&!get_calls);
 assert(!open_bound(&c,3,&sdk)&&c&&get_calls==1);
 struct anyka_encode_param before=model;struct anyka_encoder_snapshot out;
 struct anyka_encoder_settings desired={2048,4096,15,32,20,48,0};
 assert(!anyka_encoder_control_apply(c,ANYKA_ENCODER_ALL,&desired,&out));
 struct anyka_encode_param expected=before;expected.target_kbps=2048;expected.maximum_kbps=4096;
 expected.gop_frames=15;expected.initial_qp=32;expected.minimum_qp=20;expected.maximum_qp=48;expected.rate_control=0;
 assert(!memcmp(&expected,&model,sizeof model)&&!memcmp(&expected,&out.attributes,sizeof model));
 assert(set_calls==1&&idr_calls==1&&!qp_calls&&!iframe_calls&&out.changes==1&&out.iframe_valid);
 assert(out.iframe_effective.minimum_qp==26&&out.iframe_effective.maximum_qp==40);
 assert(!anyka_encoder_control_apply(c,ANYKA_ENCODER_ALL,&desired,&out)&&set_calls==1&&idr_calls==1);
 for(unsigned mode=0;mode<7;mode++){
  struct anyka_encoder_settings bad=desired;unsigned fields=ANYKA_ENCODER_ALL;
  if(mode==0)bad.target_kbps=65536;
  if(mode==1)bad.maximum_kbps=bad.target_kbps-1;
  if(mode==2)bad.gop_frames=0;
  if(mode==3)bad.gop_frames=1001;
  if(mode==4)bad.maximum_qp=52;
  if(mode==5)bad.initial_qp=bad.minimum_qp-1;
  if(mode==6)bad.rate_control=2;
  assert(anyka_encoder_control_apply(c,fields,&bad,&out)==-ERANGE&&set_calls==1);
 }
 assert(anyka_encoder_control_apply(c,16,&desired,&out)==-EINVAL&&set_calls==1);
 desired.gop_frames=60;ignore_set=1;assert(anyka_encoder_control_apply(c,ANYKA_ENCODER_GOP,&desired,&out)==-EIO&&idr_calls==1);ignore_set=0;
 set_error=0xd000001;assert(anyka_encoder_control_apply(c,ANYKA_ENCODER_GOP,&desired,&out)==-EIO&&out.sdk_result==set_error);set_error=0;
 get_error=0xd000002;assert(anyka_encoder_control_read(c,&out)==-EIO&&out.sdk_result==get_error);get_error=0;
 invalid_iframe=1;assert(!anyka_encoder_control_read(c,&out)&&!out.iframe_valid);
 /* Invalid derived iframe metadata must not block repairing ordinary bitrate. */
 assert(!anyka_encoder_control_apply(c,ANYKA_ENCODER_GOP,&desired,&out));invalid_iframe=0;
 unsigned calls=iframe_calls;struct anyka_encoder_iframe bad_frame={19,49,60,200};
 assert(anyka_encoder_control_iframe(c,&bad_frame,&out)==-ERANGE&&iframe_calls==calls);
 struct anyka_encoder_iframe requested={25,40,60,200};
 assert(!anyka_encoder_control_iframe(c,&requested,&out)&&out.iframe_request_known&&out.iframe_adjusted&&out.iframe_effective.maximum_kbytes==180);
 requested=(struct anyka_encoder_iframe){0};assert(!anyka_encoder_control_iframe(c,&requested,&out)&&!out.iframe_adjusted);
 assert(out.iframe_effective.maximum_qp==40&&out.iframe_requested.maximum_qp==0);
 requested=(struct anyka_encoder_iframe){25,40,1,481};calls=iframe_calls;
 assert(anyka_encoder_control_iframe(c,&requested,&out)==-ERANGE&&iframe_calls==calls);
 idr_error=17;assert(anyka_encoder_control_idr(c)==-EIO);anyka_encoder_control_free(c);
 reset();model.codec=2;c=NULL;assert(open_bound(&c,3,&sdk)==-EPROTO&&!c);
 puts("Anyka encoder: exact ABI/version, preserved fields, bounds, halfword iframe outputs, readback/no-op/failure ordering PASS");return 0;
}
