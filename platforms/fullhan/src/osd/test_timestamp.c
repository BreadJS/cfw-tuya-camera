#include "timestamp.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    struct tm tm={.tm_year=126,.tm_mon=9,.tm_mday=29,.tm_wday=4,.tm_hour=14,.tm_min=7,.tm_sec=9};
    char text[24];
    assert(!camera_osd_format_time(&tm,text));
    assert(!strcmp(text,"2026-10-29 Thu 14:07:09"));
    static const char *const days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    enum {STRIDE=CAMERA_OSD_TIMESTAMP_WIDTH+6,EXTENT=STRIDE*CAMERA_OSD_TIMESTAMP_HEIGHT};
    uint8_t memory[EXTENT+2];
    for (int day=0;day<7;day++) {
        tm.tm_wday=day;assert(!camera_osd_format_time(&tm,text));assert(!memcmp(text+11,days[day],3));
        memset(memory,0xa5,sizeof memory);
        assert(!camera_osd_timestamp_mask(text,memory+1,EXTENT,STRIDE));
        assert(memory[0]==0xa5 && memory[EXTENT+1]==0xa5);
        unsigned ink=0,border=0,transparent=0;
        for (unsigned y=0;y<CAMERA_OSD_TIMESTAMP_HEIGHT;y++) for (unsigned x=0;x<STRIDE;x++) {
            unsigned value=memory[1+y*STRIDE+x];assert(value<=2);
            if (x>=CAMERA_OSD_TIMESTAMP_WIDTH) assert(value==0);
            if (value==2) {
                ++ink;assert(x>0 && y>0 && x<CAMERA_OSD_TIMESTAMP_WIDTH-1 && y<CAMERA_OSD_TIMESTAMP_HEIGHT-1);
                for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++)
                    assert(memory[1+(unsigned)((int)y+dy)*STRIDE+(unsigned)((int)x+dx)]!=0);
            } else if (value==1) ++border;else ++transparent;
        }
        assert(ink>500 && border>100 && transparent>1000);
        uint8_t packed[112*CAMERA_OSD_TIMESTAMP_HEIGHT+2];memset(packed,0xa5,sizeof packed);
        assert(!camera_osd_timestamp_pack(memory+1,EXTENT,STRIDE,packed+1,sizeof packed-2,112));
        assert(packed[0]==0xa5 && packed[sizeof packed-1]==0xa5);
        for (unsigned y=0;y<CAMERA_OSD_TIMESTAMP_HEIGHT;y++) for (unsigned x=0;x<CAMERA_OSD_TIMESTAMP_WIDTH;x++)
            assert(((packed[1+y*112+x/4]>>(2*(x&3)))&3)==memory[1+y*STRIDE+x]);
    }
    tm.tm_year=100;tm.tm_mon=1;tm.tm_mday=29;assert(!camera_osd_format_time(&tm,text));
    tm.tm_year=0;memset(text,'X',sizeof text);assert(camera_osd_format_time(&tm,text)==-EINVAL && text[0]=='X');
    tm.tm_year=8100;assert(camera_osd_format_time(&tm,text)==-EINVAL);
    assert(camera_osd_format_time(NULL,text)==-EINVAL);
    strcpy(text,"2026-10-29 Thu 14:07:09");
    assert(camera_osd_timestamp_mask(text,memory,EXTENT,CAMERA_OSD_TIMESTAMP_WIDTH-1)==-EINVAL);
    assert(camera_osd_timestamp_mask(text,memory,1,STRIDE)==-EINVAL);
    text[12]='?';memset(memory,0xa5,sizeof memory);
    assert(camera_osd_timestamp_mask(text,memory,EXTENT,STRIDE)==-EINVAL && memory[0]==0xa5);
    puts("timestamp formatter/mask tests PASS; no hardware pixel format asserted");
    return 0;
}
