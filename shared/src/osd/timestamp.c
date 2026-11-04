#include "timestamp.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static const char *const weekdays[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
int camera_osd_format_time(const struct tm *tm,char out[24])
{
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if (!tm || !out || tm->tm_year < -1900 || tm->tm_year>8099 ||
        tm->tm_mon<0 || tm->tm_mon>11 || tm->tm_wday<0 || tm->tm_wday>6 ||
        tm->tm_mday<1 || tm->tm_hour<0 || tm->tm_hour>23 ||
        tm->tm_min<0 || tm->tm_min>59 || tm->tm_sec<0 || tm->tm_sec>60) return -EINVAL;
    unsigned year=(unsigned)(tm->tm_year+1900);
    unsigned limit=days[tm->tm_mon];
    if (tm->tm_mon==1 && year%4==0 && (year%100 || year%400==0)) ++limit;
    if ((unsigned)tm->tm_mday>limit) return -EINVAL;
    char text[24];
    int n=snprintf(text,sizeof text,"%04u-%02u-%02u %s %02u:%02u:%02u",year,
        (unsigned)tm->tm_mon+1u,(unsigned)tm->tm_mday,weekdays[tm->tm_wday],
        (unsigned)tm->tm_hour,(unsigned)tm->tm_min,(unsigned)tm->tm_sec);
    if (n!=23) return -ERANGE;
    memcpy(out,text,sizeof text); return 0;
}
struct glyph { char c;uint8_t rows[7]; };
/* Original small fixed glyphs, five columns and seven rows. Only the timestamp
 * alphabet is included; no external font runtime or firmware font is required. */
static const struct glyph glyphs[]={
 {'0',{14,17,19,21,25,17,14}},{'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}},{'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}},{'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}},{'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}},{'9',{14,17,17,15,1,1,14}},
 {'-',{0,0,0,31,0,0,0}},{':',{0,4,4,0,4,4,0}},
 {'S',{15,16,16,14,1,1,30}},{'M',{17,27,21,21,17,17,17}},
 {'T',{31,4,4,4,4,4,4}},{'W',{17,17,17,21,21,27,17}},
 {'F',{31,16,16,30,16,16,16}},
 {'u',{0,0,17,17,17,19,13}},{'n',{0,0,30,17,17,17,17}},
 {'o',{0,0,14,17,17,17,14}},{'e',{0,0,14,17,31,16,15}},
 {'d',{1,1,15,17,17,17,15}},{'h',{16,16,30,17,17,17,17}},
 {'r',{0,0,22,25,16,16,16}},{'i',{4,0,12,4,4,4,14}},
 {'a',{0,0,14,1,15,17,15}},{'t',{8,8,30,8,8,9,6}},
 {' ',{0,0,0,0,0,0,0}}
};
static const uint8_t *glyph(char c)
{
    for (size_t i=0;i<sizeof glyphs/sizeof glyphs[0];i++) if (glyphs[i].c==c) return glyphs[i].rows;
    return NULL;
}
int camera_osd_timestamp_mask(const char text[24],uint8_t *out,size_t bytes,size_t stride)
{
    if (!text || !out || stride<CAMERA_OSD_TIMESTAMP_WIDTH ||
        stride>bytes/CAMERA_OSD_TIMESTAMP_HEIGHT || text[23]) return -EINVAL;
    for (unsigned i=0;i<23;i++) if (!text[i] || !glyph(text[i])) return -EINVAL;
    if (text[4]!='-' || text[7]!='-' || text[10]!=' ' || text[14]!=' ' || text[17]!=':' || text[20]!=':') return -EINVAL;
    int weekday=0; for (unsigned i=0;i<7;i++) if (!memcmp(text+11,weekdays[i],3)) weekday=1;
    if (!weekday) return -EINVAL;
    for (unsigned i=0;i<23;i++) if (i!=4 && i!=7 && i!=10 && (i<11 || i>13) && i!=14 && i!=17 && i!=20 && (text[i]<'0'||text[i]>'9')) return -EINVAL;
    memset(out,0,stride*CAMERA_OSD_TIMESTAMP_HEIGHT);
    for (unsigned ch=0;ch<23;ch++) {
        const uint8_t *rows=glyph(text[ch]);
        for (unsigned y=0;y<7;y++) for (unsigned x=0;x<5;x++) if (rows[y]&(1u<<(4-x)))
            for (unsigned dy=0;dy<3;dy++) for (unsigned dx=0;dx<3;dx++)
                out[(2+y*3+dy)*stride+2+ch*18+x*3+dx]=2;
    }
    for (unsigned y=1;y<CAMERA_OSD_TIMESTAMP_HEIGHT-1;y++) for (unsigned x=1;x<CAMERA_OSD_TIMESTAMP_WIDTH-1;x++)
        if (out[y*stride+x]==2)
            for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++) {
                uint8_t *pixel=&out[(size_t)((int)y+dy)*stride+(unsigned)((int)x+dx)];
                if (!*pixel) *pixel=1;
            }
    return 0;
}
int camera_osd_timestamp_pack(const uint8_t *mask,size_t mask_bytes,size_t mask_stride,
                              uint8_t *out,size_t out_bytes,size_t out_stride)
{
    if (!mask || !out || mask_stride<CAMERA_OSD_TIMESTAMP_WIDTH ||
        mask_stride>mask_bytes/CAMERA_OSD_TIMESTAMP_HEIGHT ||
        out_stride<(CAMERA_OSD_TIMESTAMP_WIDTH+3u)/4u || (out_stride&7u) ||
        out_stride>out_bytes/CAMERA_OSD_TIMESTAMP_HEIGHT) return -EINVAL;
    for (unsigned y=0;y<CAMERA_OSD_TIMESTAMP_HEIGHT;y++) for (unsigned x=0;x<CAMERA_OSD_TIMESTAMP_WIDTH;x++)
        if (mask[y*mask_stride+x]>2) return -EINVAL;
    memset(out,0,out_stride*CAMERA_OSD_TIMESTAMP_HEIGHT);
    for (unsigned y=0;y<CAMERA_OSD_TIMESTAMP_HEIGHT;y++) for (unsigned x=0;x<CAMERA_OSD_TIMESTAMP_WIDTH;x++)
        out[y*out_stride+x/4u]|=(uint8_t)(mask[y*mask_stride+x]<<(2u*(x&3u)));
    return 0;
}
