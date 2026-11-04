#define _GNU_SOURCE
#include "../../src/anyka/application.c"
#include <assert.h>
static unsigned actions,calibration_calls;
static int calibration_rc;
int anyka_board_calibrate(struct anyka_board *board){assert(board);calibration_calls++;return calibration_rc;}
static int snapshot_rc,command_rc;
static enum anyka_board_action fail_action;
static struct anyka_board_status actual={.day_mode=1,.ir_led=0,.status_led=1,.motors_available=1,.pan={.position=100},.tilt={.position=200}};
static enum anyka_board_action last_action;
static int last_value;
int anyka_executor_status(struct anyka_executor *e,struct anyka_board_status *s)
{assert(e);*s=actual;return snapshot_rc;}
int anyka_executor_board(struct anyka_executor *e,enum anyka_board_action action,int value)
{assert(e);last_action=action;last_value=value;actions++;if(command_rc&&(!fail_action||fail_action==action))return command_rc;if(action==ANYKA_BOARD_DAY)actual.day_mode=value;else if(action==ANYKA_BOARD_IR)actual.ir_led=value;else if(action==ANYKA_BOARD_LED)actual.status_led=value;return 0;}
struct input {const char *body;size_t used;};
static ssize_t read_form(void *opaque,void *out,size_t cap)
{struct input *i=opaque;size_t left=strlen(i->body)-i->used;if(left>cap)left=cap;memcpy(out,i->body+i->used,left);i->used+=left;return(ssize_t)left;}
static struct control_reply request(struct application *a,const char *method,const char *path,const char *body)
{struct input in={body?body:"",0};struct control_request q={method,path,"application/x-www-form-urlencoded",{&in,strlen(in.body),read_form}};struct control_reply r;board_route(a,&q,&r);return r;}
int main(void)
{
 char configpath[]="/tmp/ptz-app-board-XXXXXX";assert(mkdtemp(configpath));int config=open(configpath,O_RDONLY|O_DIRECTORY);assert(config>=0);
 char lockpath[512];assert(snprintf(lockpath,sizeof lockpath,"%s/application.lock",configpath)>0);int owner=application_lock_path(lockpath);assert(owner>=0&&(fcntl(owner,F_GETFD)&FD_CLOEXEC));assert(application_lock_path(lockpath)==-EWOULDBLOCK);close(owner);owner=application_lock_path(lockpath);assert(owner>=0);close(owner);assert(!unlink(lockpath));
 struct application boot={.executor=(struct anyka_executor*)1,.reset_directory=config};atomic_init(&boot.board_ready_mask,0);atomic_init(&boot.board_settings_error,0);
 actual.day_mode=0;actual.ir_led=1;actual.status_led=1;startup_physical_settings(&boot);assert(actual.day_mode==1&&!actual.ir_led&&!actual.status_led&&atomic_load(&boot.board_ready_mask)==7&&!atomic_load(&boot.board_settings_error));
 struct application partial={.executor=(struct anyka_executor*)1,.reset_directory=config};atomic_init(&partial.board_ready_mask,0);atomic_init(&partial.board_settings_error,0);command_rc=-EIO;fail_action=ANYKA_BOARD_IR;startup_physical_settings(&partial);assert(atomic_load(&partial.board_ready_mask)==5&&atomic_load(&partial.board_settings_error)==-EIO);command_rc=0;fail_action=0;
 struct anyka_board_settings replay={0,1,1};assert(!anyka_board_settings_save(config,&replay));actual.day_mode=1;actual.ir_led=0;actual.status_led=0;startup_physical_settings(&boot);assert(!actual.day_mode&&actual.ir_led&&actual.status_led);
 actual.day_mode=1;actual.ir_led=0;actual.status_led=1;replay=(struct anyka_board_settings){1,0,1};assert(!anyka_board_settings_save(config,&replay));actions=0;
 struct application a={.executor=(struct anyka_executor*)1,.board=(struct anyka_board*)1,.reset_directory=config,.board_settings={1,0,1}};struct control_reply r;
 atomic_init(&a.board_ready_mask,7);atomic_init(&a.board_settings_error,0);
 unsetenv("CAMERA_PTZ_CALIBRATE");startup_calibration(&a);assert(!calibration_calls&&!a.calibration_attempted);
 setenv("CAMERA_PTZ_CALIBRATE","0",1);startup_calibration(&a);assert(!calibration_calls);
 setenv("CAMERA_PTZ_CALIBRATE","true",1);startup_calibration(&a);assert(!calibration_calls);
 setenv("CAMERA_PTZ_CALIBRATE","1",1);startup_calibration(&a);assert(calibration_calls==1&&!a.calibration_error);startup_calibration(&a);assert(calibration_calls==1);
 struct application failed={.executor=(struct anyka_executor*)1,.board=(struct anyka_board*)1};calibration_rc=-EIO;startup_calibration(&failed);assert(calibration_calls==2&&failed.calibration_error==-EIO);startup_calibration(&failed);assert(calibration_calls==2);
 r=request(&failed,"GET","/api/ptz",NULL);assert(r.status==200&&strstr(r.json,"\"available\":false")&&strstr(r.json,"\"calibration_failed\":true"));
 r=request(&failed,"POST","/api/ptz","direction=left");assert(r.status==503&&!actions);
 r=request(&failed,"GET","/api/illumination",NULL);assert(r.status==200&&strstr(r.json,"\"available\":true"));
 unsetenv("CAMERA_PTZ_CALIBRATE");
 r=request(&a,"GET","/api/illumination",NULL);assert(r.status==200&&strstr(r.json,"\"filter\":\"day\""));
 r=request(&a,"POST","/api/illumination","filter=night&ir_led=true");assert(r.status==200&&actions==2&&last_action==ANYKA_BOARD_IR&&last_value==1);
 r=request(&a,"POST","/api/illumination","filter=unknown&ir_led=true");assert(r.status==400&&actions==2);
 r=request(&a,"GET","/api/indicator",NULL);assert(r.status==200&&strstr(r.json,"\"mode\":\"on\""));
 r=request(&a,"POST","/api/indicator","mode=green");assert(r.status==400);r=request(&a,"POST","/api/indicator","mode=off");assert(r.status==200&&last_action==ANYKA_BOARD_LED&&!last_value);
 r=request(&a,"GET","/api/ptz",NULL);assert(r.status==200&&strstr(r.json,"\"moving\":false"));
 r=request(&a,"POST","/api/ptz","direction=left");assert(r.status==200&&last_action==ANYKA_BOARD_MOVE&&last_value==1);
 r=request(&a,"POST","/api/ptz","direction=left&hold=true");assert(r.status==200&&last_action==ANYKA_BOARD_HOLD&&last_value==1);
 r=request(&a,"POST","/api/ptz","direction=left&hold=false");assert(r.status==200&&last_action==ANYKA_BOARD_MOVE);
 r=request(&a,"POST","/api/ptz","direction=left&hold=invalid");assert(r.status==400);
 r=request(&a,"POST","/api/ptz","direction=stop");assert(r.status==200&&last_action==ANYKA_BOARD_STOP);
 r=request(&a,"POST","/api/ptz","direction=calibrate");assert(r.status==400);
 command_rc=-ERANGE;r=request(&a,"POST","/api/ptz","direction=right");assert(r.status==503);command_rc=0;
 struct anyka_board_settings saved;
 command_rc=-EIO;fail_action=ANYKA_BOARD_IR;r=request(&a,"POST","/api/illumination","filter=day&ir_led=true");assert(r.status==503&&strstr(r.json,"\"partial_apply\":true"));assert(!anyka_board_settings_load(config,&saved)&&saved.day_mode==1);assert(atomic_load(&a.board_settings_error)==-EIO&&!(atomic_load(&a.board_ready_mask)&2));
 command_rc=0;fail_action=0;r=request(&a,"POST","/api/illumination","filter=day&ir_led=true");assert(r.status==200&&!atomic_load(&a.board_settings_error)&&atomic_load(&a.board_ready_mask)==7);
 snapshot_rc=-EIO;unsigned before=actions;r=request(&a,"POST","/api/indicator","mode=on");assert(r.status==503&&actions==before);r=request(&a,"GET","/api/ptz",NULL);assert(r.status==200&&strstr(r.json,"\"available\":false"));
 anyka_video_mode_defaults(&a.video_mode);
 {struct input in={"width=1280&height=720&fps=25&profile=2",0};struct control_request q={"POST","/api/video-mode","application/x-www-form-urlencoded",{&in,strlen(in.body),read_form}};video_mode_route(&a,&q,&r);assert(r.status==200&&strstr(r.json,"\"restart_required\":true"));assert(a.video_mode.width==1920&&a.video_mode.fps==15);struct anyka_video_mode savedmode;assert(!anyka_video_mode_load(config,&savedmode)&&savedmode.width==1280&&savedmode.fps==25&&savedmode.profile==2);}
 {struct input in={"width=1280&height=720&fps=60&profile=2",0};struct control_request q={"POST","/api/video-mode","application/x-www-form-urlencoded",{&in,strlen(in.body),read_form}};video_mode_route(&a,&q,&r);assert(r.status==400);}
 rtsp_active=0;quiesce_signal(SIGUSR1);assert(quiesce_requested);quiesce_requested=0;
 int pending=openat(config,"reset.pending",O_WRONLY|O_CREAT|O_EXCL,0600);assert(pending>=0&&write(pending,"invalid\n",8)==8);close(pending);assert(complete_pending_reset(config)==-EPROTO);
 pending=openat(config,"reset.pending",O_WRONLY|O_TRUNC);assert(pending>=0&&write(pending,"pending\n",8)==8);close(pending);
 assert(!mkdirat(config,"update",0700));int updates=openat(config,"update",O_RDONLY|O_DIRECTORY);assert(updates>=0);int accepted=openat(updates,"accepted",O_WRONLY|O_CREAT|O_EXCL,0600);assert(accepted>=0&&write(accepted,"9\n",2)==2);close(accepted);
 assert(!complete_pending_reset(config));assert(faccessat(config,"video-mode.conf",F_OK,0)<0&&errno==ENOENT);assert(faccessat(config,"board.conf",F_OK,0)<0&&errno==ENOENT);assert(!faccessat(updates,"accepted",F_OK,0));assert(!complete_pending_reset(config));
 assert(!unlinkat(updates,"accepted",0));close(updates);assert(!unlinkat(config,"update",AT_REMOVEDIR));close(config);assert(!rmdir(configpath));
 puts("PASS application board API: cached status, executor-only bounded commands, target LED modes, validation, failure propagation and deferred quiesce signal");
}
