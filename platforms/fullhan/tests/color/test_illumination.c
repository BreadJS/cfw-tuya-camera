#include "../../src/native/illumination/controller.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fake {unsigned p19,p20,led,sleeps,peak19,peak20,color_writes;uint32_t color;int owner,fail_color;};
static int owner(void *p){return ((struct fake *)p)->owner?0:-EPERM;}
static int pin(void *p,unsigned n){struct fake*f=p;assert(n==19||n==20);if(n==19)f->p19=0;else f->p20=0;return 0;}
static int write_pin(void*p,unsigned n,unsigned v){struct fake*f=p;assert(n==19||n==20);if(n==19){f->p19=v;if(v)++f->peak19;}else{f->p20=v;if(v)++f->peak20;}return 0;}
static int delay(void*p,unsigned us){struct fake*f=p;assert(us==300000);assert((f->p19^f->p20)==1);assert(!f->led);++f->sleeps;return 0;}
static int led_prepare(void*p){(void)p;return 0;}
static int led_write(void*p,unsigned on){((struct fake*)p)->led=on;return 0;}
static int read_color(void*p,uint32_t offset,uint32_t*out){assert(offset==0x5d0);*out=((struct fake*)p)->color;return 0;}
static int write_color(void*p,uint32_t offset,uint32_t value){struct fake*f=p;assert(offset==0x5d0);if(f->fail_color)return -EIO;f->color=value;++f->color_writes;return 0;}
static int settling(void *p,uint32_t us){assert(!((struct fake*)p)->led);assert(us==50000U||us==500000U);return 0;}
int main(void)
{
    struct fake f={.color=0xa5a50100,.owner=1};struct illumination s;
    struct ircut_io cut={&f,owner,pin,write_pin,delay};struct ir_led_io led={&f,led_prepare,led_write};struct illumination_color_io color={&f,read_color,write_color,settling};
    assert(!illumination_init(&s,&cut,&f,owner,&led,&color));assert(!f.sleeps);
    assert(!illumination_prepare(&s)&&!f.led&&!f.p19&&!f.p20);
    char json[1024];assert(!illumination_status_json(&s,json,sizeof json)&&strstr(json,"\"filter\":\"unknown\""));
    assert(illumination_color_begin(&s)==0); /* Unknown detent: hold AWB. */
    assert(!illumination_apply(&s,1,1));assert(f.led&&f.peak19==1&&!f.peak20&&f.sleeps==1&&!f.p19&&!f.p20);
    assert(illumination_color_begin(&s)==0);
    assert(f.color==0xa5a50000);assert(!illumination_status_json(&s,json,sizeof json)&&strstr(json,"\"monochrome\":true"));
    assert(illumination_apply(&s,0,0)==-EAGAIN&&f.sleeps==1&&f.led);
    assert(!illumination_apply(&s,1,0)&&f.sleeps==1&&!f.led);
    s.last_pulse_ms=1;assert(!illumination_apply(&s,0,0));assert(f.peak20==1&&f.sleeps==2&&f.color==0xa5a50100);
    assert(illumination_color_begin(&s)==1);
    assert(illumination_color_begin(&s)==0); /* HTTP transition cannot overlap. */
    illumination_color_end(&s);
    assert(!illumination_apply(&s,0,1));assert(illumination_color_begin(&s)==0);
    assert(!illumination_apply(&s,0,0));
    s.last_pulse_ms=1;f.fail_color=1;assert(illumination_apply(&s,1,1)==-EIO&&!f.led&&!f.p19&&!f.p20);
    assert(!illumination_status_json(&s,json,sizeof json)&&strstr(json,"\"last_error\":-5"));
    puts("illumination: exact pulse direction, LED-off sequencing, saturation mask/restore, cooldown, LED-only idempotence, safe error state passed");return 0;
}
