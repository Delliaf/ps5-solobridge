/* GPL-3.0. Local controller, persistence and file fallback.
 * API references: PS5 SDK sce_stubs and octet/src/ps5_vpad.c.
 * No injection, privilege changes or synthetic input. */
#include "control.h"
#include "bridge_logic.h"
#include <ps5/kernel.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define DIR "/data/solobridge"
#define STATE DIR "/volume"
#define REQUEST DIR "/volume.request"
#define STOP DIR "/stop"
static atomic_int desired=TTGK_DEFAULT,applied=-1,quitting=0;
static pthread_t worker;
static int started;
int sceKernelLoadStartModule(const char *,size_t,const void *,uint32_t,void *,int *);
typedef int (*init_fn)(void);
typedef int (*user_init_fn)(void *);
typedef int (*user_fn)(int *);
typedef int (*get_fn)(int,int,int);
typedef int (*open_fn)(int,int,int,const void *);
typedef int (*close_fn)(int);
typedef int (*read_fn)(int,void *);
static user_fn get_user;
static get_fn get_handle;
static open_fn open_pad;
static close_fn close_pad;
static read_fn read_pad;
typedef struct { uint32_t buttons; uint8_t reserved[72],connected,rest[43]; } pad_data;
_Static_assert(sizeof(pad_data)==120,"pad ABI");
typedef struct { char reserved[45],message[3075]; } notify_request;
int sceKernelSendNotificationRequest(int,notify_request *,size_t,int);
static uint64_t ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int load(const char *name) {
  char path[128]; int res=0;
  snprintf(path,sizeof(path),"/system/common/lib/%s.sprx",name);
  return sceKernelLoadStartModule(path,0,NULL,0,NULL,&res);
}
static void *resolve(int m,const char *nid) {
  return (void *)(intptr_t)kernel_dynlib_resolve(getpid(),(uint32_t)m,nid);
}
static int pad_init(void) {
  if (load("libSceIpmi")<0 || load("libSceMbus")<0) return -1;
  int u=load("libSceUserService"),p=load("libScePad");
  if (u<0 || p<0) return -1;
  user_init_fn ui=(user_init_fn)resolve(u,"j3YMu1MVNNo");
  get_user=(user_fn)resolve(u,"eNb53LQJmIM");
  init_fn pi=(init_fn)resolve(p,"hv1luiJrqQM");
  get_handle=(get_fn)resolve(p,"u1GRHp+oWoY");
  open_pad=(open_fn)resolve(p,"xk0AcarP3V4");
  close_pad=(close_fn)resolve(p,"6ncge5+l5Qs");
  read_pad=(read_fn)resolve(p,"YndgXqQVV7c");
  if (!ui||!get_user||!pi||!get_handle||!open_pad||!close_pad||!read_pad) return -1;
  int r=ui(NULL);
  if (r<0) fprintf(stderr,"pad: user init %#x\n",(unsigned)r);
  r=pi();
  if (r<0) { fprintf(stderr,"pad: init %#x\n",(unsigned)r); return -1; }
  return 0;
}
static int read_value(const char *path,int *v) {
  FILE *f=fopen(path,"r"); if (!f) return -1;
  char line[64],tail; int value=-1;
  int ok=fgets(line,sizeof(line),f)!=NULL &&
         sscanf(line,"%d %c",&value,&tail)==1 && volume_valid(value);
  if (ok && !strchr(line,'\n') && !feof(f)) ok=0;
  if (ok && fgetc(f)!=EOF) ok=0;
  fclose(f);
  if (!ok) return -1;
  *v=value; return 0;
}
static int save_value(int v) {
  FILE *f=fopen(STATE ".tmp","w"); if (!f) return -1;
  int ok=fprintf(f,"%d\n",v)>0 && fflush(f)==0 && fsync(fileno(f))==0;
  if (fclose(f)!=0) ok=0;
  if (ok && rename(STATE ".tmp",STATE)==0) return 0;
  unlink(STATE ".tmp"); return -1;
}
static void notify_volume(int v) {
  notify_request req={0};
  if (v&TTGK_MUTE) snprintf(req.message,sizeof(req.message),"TTGK USB Audio: muted");
  else snprintf(req.message,sizeof(req.message),"TTGK USB Audio: %.1f dB",(v&255)*0.5-74.0);
  sceKernelSendNotificationRequest(0,&req,sizeof(req),0);
}
static void *control_worker(void *arg) {
  (void)arg;
  int pad_ok=pad_init()==0;
  if (!pad_ok) fprintf(stderr,"TTGK: direct DualSense API unavailable; use volume.request\n");
  int handle=-1,owned=0,user=-1,last=-1,persisted=-1,current;
  if (read_value(STATE,&persisted)<0) persisted=-1;
  uint64_t retry=0,user_check=0,files_at=0,changed=0,notify_at=0;
  hotkey_t h={0}; int released=0;
  while (!atomic_load(&quitting)) {
    uint64_t t=ms();
    if (pad_ok && t>=user_check) {
      user_check=t+1000;
      int new_user=-1; if (get_user(&new_user)<0) new_user=-1;
      if (new_user!=user) {
        if (handle>=0 && owned) close_pad(handle);
        handle=-1; owned=0; user=new_user; retry=0; h=(hotkey_t){0}; released=0;
      }
    }
    if (pad_ok && handle<0 && user>0 && t>=retry) {
      retry=t+5000; handle=get_handle(user,0,0);
      if (handle<0) { handle=open_pad(user,0,0,NULL); owned=handle>=0; }
      if (handle<0) fprintf(stderr,"TTGK: pad handle unavailable %#x\n",(unsigned)handle);
      else fprintf(stderr,"TTGK: pad handle ready\n");
    }
    if (handle>=0) {
      union { pad_data d; uint64_t align; uint8_t storage[512]; } state={0};
      int r=read_pad(handle,&state);
      if (r<0 || !state.d.connected) {
        if (owned) close_pad(handle);
        handle=-1; owned=0; retry=t+1000; h=(hotkey_t){0}; released=0;
      } else {
        uint32_t b=state.d.buttons;
        if ((b&PAD_MOD)!=PAD_MOD) released=1;
        if (released) atomic_store(&desired,hotkey(&h,b,t,atomic_load(&desired)));
      }
    }
    if (t>=files_at) {
      files_at=t+1000;
      if (unlink(STOP)==0) { atomic_store(&quitting,1); break; }
      if (rename(REQUEST,REQUEST ".processing")==0) {
        if (read_value(REQUEST ".processing",&current)==0) atomic_store(&desired,current);
        else fprintf(stderr,"TTGK: invalid volume.request\n");
        unlink(REQUEST ".processing");
      }
    }
    int a=atomic_load(&applied);
    if (a>=0 && a!=last) {
      last=a; changed=t;
      if (t>=notify_at) { notify_volume(a); notify_at=t+600; }
    }
    if (last>=0 && last!=persisted && t-changed>=1000) {
      if (save_value(last)==0) persisted=last;
      else changed=t;
    }
    usleep(40000);
  }
  if (handle>=0 && owned) close_pad(handle);
  int a=atomic_load(&applied);
  if (a>=0 && a!=persisted) save_value(a);
  return NULL;
}
int control_start(void) {
  int v; if (read_value(STATE,&v)==0) atomic_store(&desired,v);
  int r=pthread_create(&worker,NULL,control_worker,NULL);
  if (r) { fprintf(stderr,"TTGK: control thread failed (%d)\n",r); return -1; }
  started=1; return 0;
}
int control_volume(void) { return atomic_load(&desired); }
void control_applied(int v) { atomic_store(&applied,v); }
int control_stop_requested(void) { return atomic_load(&quitting); }
void control_stop(void) {
  atomic_store(&quitting,1);
  if (started) pthread_join(worker,NULL);
}
