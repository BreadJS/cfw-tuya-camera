#include "graphv2.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct camera_graphv2_bitmap b={.physical=0xa3700000,.bytes=4096,.width=418,.height=26,
        .stride=112,.x=16,.y=16,.slot=0,.frame_width=1920,.frame_height=1080};
    uint32_t words[CAMERA_GRAPHV2_WORDS];
    assert(!camera_graphv2_timestamp(&b,1,words));
    assert(words[0]==1 && words[1]==0 && words[2]==1 && words[3]==2 && words[4]==b.physical);
    assert(words[9]==2 && words[13]==418 && words[14]==26 && words[15]==16 && words[16]==16 && words[17]==112);
    assert(words[18]==0 && words[19]==0 && words[20]==0xff000000 && words[21]==0xffffffff && words[22]==0);
    for (unsigned i=23;i<CAMERA_GRAPHV2_WORDS;i++) assert(!words[i]);
    b.bytes=1;memset(words,0xa5,sizeof words);
    assert(camera_graphv2_timestamp(&b,1,words)==-ERANGE && words[0]==0xa5a5a5a5);
    b.bytes=4096;b.x=17;assert(camera_graphv2_timestamp(&b,1,words)==-ERANGE);
    b.x=16;b.physical=0xa37ff800;assert(camera_graphv2_timestamp(&b,1,words)==-ERANGE);
    b.physical=0xa3700000;b.frame_width=320;assert(camera_graphv2_timestamp(&b,1,words)==-ERANGE);
    assert(!camera_graphv2_timestamp(&b,0,words) && words[0]==1 && words[2]==0);
    for (unsigned i=3;i<CAMERA_GRAPHV2_WORDS;i++) assert(words[i]==0);
    puts("exact-camera GraphV2 packet tests PASS; no ioctl submitted");return 0;
}
