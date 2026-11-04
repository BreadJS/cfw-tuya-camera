#define _GNU_SOURCE
#include "../camera_rtsp.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Synthetic Annex-B fixtures only. This is deliberately not a capture backend. */
#define MAX_FILE (4u*1024u*1024u)
#define MAX_FRAMES 2048
struct fixture {
    unsigned char *video, *audio;
    size_t video_size,audio_size,offset[MAX_FRAMES],length[MAX_FRAMES],count;
    int key[MAX_FRAMES];
    uint64_t epoch,video_index,audio_index;
    unsigned fps,rate,audio_chunk;
    unsigned char audio_buffer[CAMERA_RTSP_MAX_AUDIO];
};
static unsigned char *read_file(const char *name,size_t *size)
{
    FILE *f=fopen(name,"rb");
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||ftell(f)<=0||ftell(f)>(long)MAX_FILE){fclose(f);return NULL;}
    *size=(size_t)ftell(f);rewind(f);
    unsigned char *p=malloc(*size);
    if(!p||fread(p,1,*size,f)!=*size){free(p);p=NULL;}
    fclose(f);return p;
}
static size_t startcode(const unsigned char *p,size_t n,size_t at,size_t *prefix)
{
    for(size_t i=at;i+3<=n;i++) {
        if(p[i]||p[i+1])continue;
        if(p[i+2]==1){*prefix=3;return i;}
        if(i+4<=n&&!p[i+2]&&p[i+3]==1){*prefix=4;return i;}
    }
    return n;
}
static int index_video(struct fixture *f,struct camera_media_description *d)
{
    size_t prefix,at=startcode(f->video,f->video_size,0,&prefix),begin=0;
    int have_frame=0,key=0;
    while(at<f->video_size) {
        size_t payload=at+prefix,nextprefix=0;
        size_t next=startcode(f->video,f->video_size,payload,&nextprefix);
        if(payload>=next)return -1;
        unsigned type=f->video[payload]&31;
        if(type==9&&have_frame) {
            if(f->count>=MAX_FRAMES)return -1;
            f->offset[f->count]=begin;f->length[f->count]=at-begin;f->key[f->count++]=key;
            begin=at;key=0;have_frame=0;
        }
        if(type==5)key=1;
        if(type==1||type==5)have_frame=1;
        if(type==7&&!d->sps){d->sps=f->video+payload;d->sps_size=next-payload;}
        if(type==8&&!d->pps){d->pps=f->video+payload;d->pps_size=next-payload;}
        at=next;prefix=nextprefix;
    }
    if(have_frame&&f->count<MAX_FRAMES) {
        f->offset[f->count]=begin;f->length[f->count]=f->video_size-begin;f->key[f->count++]=key;
    }
    for(size_t i=0;i<f->count;i++)if(f->length[i]>CAMERA_RTSP_MAX_AU)return -1;
    return f->count&&d->sps&&d->pps?0:-1;
}
static int next_video(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct fixture *f=opaque;
    uint64_t pts=f->epoch+f->video_index*1000000u/f->fps;
    if(now<pts)return 0;
    size_t i=(size_t)(f->video_index++%f->count);
    *frame=(struct camera_media_frame){f->video+f->offset[i],f->length[i],pts,f->key[i],0,0};
    return 1;
}
static int next_audio(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct fixture *f=opaque;
    uint64_t pts=f->epoch+f->audio_index*20000u;
    if(now<pts)return 0;
    size_t offset=(size_t)((f->audio_index++*f->audio_chunk)%f->audio_size);
    for(unsigned i=0;i<f->audio_chunk;i++)f->audio_buffer[i]=f->audio[(offset+i)%f->audio_size];
    *frame=(struct camera_media_frame){f->audio_buffer,f->audio_chunk,pts,0,0,0};return 1;
}
static int credentials_file(const char *path,char *buffer,size_t capacity)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(st.st_mode&077)||st.st_size<=0||st.st_size>=(off_t)capacity){close(fd);return -1;}
    ssize_t n=read(fd,buffer,capacity-1);close(fd);
    if(n!=st.st_size)return -1;
    buffer[n]=0;while(n>0&&(buffer[n-1]=='\n'||buffer[n-1]=='\r'))buffer[--n]=0;
    return strchr(buffer,':')?0:-1;
}
int main(int argc,char **argv)
{
    const char *video=NULL,*audio=NULL,*codec=NULL,*auth=NULL;
    struct fixture f={.fps=15,.rate=16000};
    struct camera_rtsp_config config={"127.0.0.1",8554,"/main",NULL,4,1000};
    for(int i=1;i<argc;i++) {
        if(i+1>=argc){fprintf(stderr,"Missing option value\n");return 2;}
        const char *value=argv[++i];
        if(!strcmp(argv[i-1],"--video"))video=value;
        else if(!strcmp(argv[i-1],"--audio"))audio=value;
        else if(!strcmp(argv[i-1],"--audio-codec"))codec=value;
        else if(!strcmp(argv[i-1],"--audio-rate"))f.rate=(unsigned)atoi(value);
        else if(!strcmp(argv[i-1],"--fps"))f.fps=(unsigned)atoi(value);
        else if(!strcmp(argv[i-1],"--port"))config.port=(unsigned)atoi(value);
        else if(!strcmp(argv[i-1],"--bind"))config.bind_address=value;
        else if(!strcmp(argv[i-1],"--path"))config.path=value;
        else if(!strcmp(argv[i-1],"--auth-file"))auth=value;
        else {fprintf(stderr,"Unknown option\n");return 2;}
    }
    char credentials[512];
    if(auth){if(credentials_file(auth,credentials,sizeof credentials)){fputs("Invalid private credentials file\n",stderr);return 2;}config.credentials=credentials;}
    if(!video||!f.fps||f.fps>60||!(f.video=read_file(video,&f.video_size))){fputs("Need --video Annex-B fixture with AUD NALs and --fps 1..60\n",stderr);return 2;}
    struct camera_media_provider provider={.opaque=&f,.video=next_video};
    if(index_video(&f,&provider.description)){free(f.video);fputs("Invalid/oversized fixture\n",stderr);return 2;}
    if(audio) {
        if(!codec||!(f.audio=read_file(audio,&f.audio_size))){free(f.video);return 2;}
        if(!strcmp(codec,"pcma")){provider.description.audio_codec=CAMERA_AUDIO_PCMA;f.rate=8000;f.audio_chunk=160;}
        else if(!strcmp(codec,"l16")&&f.rate>=8000&&f.rate<=48000){provider.description.audio_codec=CAMERA_AUDIO_L16;f.audio_chunk=f.rate/50*2;}
        else {free(f.video);free(f.audio);return 2;}
        provider.description.audio_rate=f.rate;provider.audio=next_audio;
    }
    f.epoch=camera_monotonic_us();
    fprintf(stderr,"Synthetic fixture RTSP server on %s:%u%s; no hardware capture\n",config.bind_address,config.port,config.path);
    int result=camera_rtsp_run(&config,&provider);
    free(f.video);free(f.audio);return result?1:0;
}
