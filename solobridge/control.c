/* GPL-3.0. Local controller, persistence and file fallback.
 * API references: PS5 SDK sce_stubs and octet/src/ps5_vpad.c.
 * No injection, privilege changes or synthetic input. */
#include "control.h"
#include "bridge_logic.h"
#include "log.h"
#include <ps5/kernel.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define DIR "/data/solobridge"
#define STATE DIR "/volume"
#define REQUEST DIR "/volume.request"
#define STOP DIR "/stop"
#define ONLINE 1024
static atomic_int pad_stage,pad_err,pad_handle=-1,pad_user=-1,pad_connected,pad_phase;
static atomic_uint pad_buttons,pad_last_pressed,pad_changes,pad_reads;
static atomic_uint_fast64_t pad_last_ms,pad_trace_until;
static void pad_report(int stage,int err) { atomic_store(&pad_stage,stage); atomic_store(&pad_err,err); }
#define MAX_FILE DIR "/max_volume"
static int max_volume=136; /* -6 dB by default; 148 restores full range */
static atomic_int desired=TTGK_DEFAULT,applied=-1,quitting=0,pad_ready=0;
static int clamp_volume(int v) { return (v&255)>max_volume?(v&TTGK_MUTE)|max_volume:v; }
static int request_volume(int v) {
  int old=atomic_load(&desired);
  while (old&ONLINE) {
    int n=ONLINE|clamp_volume(v);
    if (atomic_compare_exchange_weak(&desired,&old,n)) return 1;
  }
  return 0;
}
static pthread_t worker,pad_worker;
static int started,pad_started;
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
  char path[128],base[80]; int res=0; uint32_t h=0;
  snprintf(base,sizeof(base),"%s.sprx",name);
  if (kernel_dynlib_handle(getpid(),base,&h)==0 && h) return (int)h;
  snprintf(path,sizeof(path),"/system/common/lib/%s",base);
  int r=sceKernelLoadStartModule(path,0,NULL,0,NULL,&res);
  if (r<0 && kernel_dynlib_handle(getpid(),base,&h)==0 && h) return (int)h;
  return r;
}
static void *resolve(int m,const char *nid) {
  return (void *)(intptr_t)kernel_dynlib_resolve(getpid(),(uint32_t)m,nid);
}
static int pad_init(void) {
  /* Capture has already loaded libSceIpmi. */
  int m=load("libSceMbus");
  if (m<0) { pad_report(2,m); return -1; }
  int u=load("libSceUserService"),p=load("libScePad");
  if (u<0 || p<0) { pad_report(3,u<0?u:p); return -1; }
  user_init_fn ui=(user_init_fn)resolve(u,"j3YMu1MVNNo");
  get_user=(user_fn)resolve(u,"eNb53LQJmIM");
  init_fn pi=(init_fn)resolve(p,"hv1luiJrqQM");
  get_handle=(get_fn)resolve(p,"u1GRHp+oWoY");
  open_pad=(open_fn)resolve(p,"xk0AcarP3V4");
  close_pad=(close_fn)resolve(p,"6ncge5+l5Qs");
  read_pad=(read_fn)resolve(p,"YndgXqQVV7c");
  if (!ui||!get_user||!pi||!get_handle||!open_pad||!close_pad||!read_pad) { pad_report(4,-1); return -1; }
  int r=ui(NULL);
  if (r<0) log_line("pad: user init %#x",(unsigned)r);
  r=pi();
  if (r<0) { pad_report(5,r); log_line("pad: init %#x",(unsigned)r); return -1; }
  pad_report(6,0);
  return 0;
}
static int read_value(const char *path,int *v) {
  FILE *fp=fopen(path,"r"); if (!fp) return -1;
  char line[64];
  size_t n=fread(line,1,sizeof(line)-1,fp);
  int ok=!ferror(fp) && feof(fp) && n>0 && !memchr(line,0,n);
  fclose(fp);
  if (!ok) return -1;
  line[n]=0;
  char *end; errno=0;
  long value=strtol(line,&end,10);
  if (errno || end==line || value<0 || value>511) return -1;
  while (isspace((unsigned char)*end)) end++;
  if (*end || !volume_valid((int)value)) return -1;
  *v=(int)value; return 0;
}
static int save_value(int v) {
  FILE *f=fopen(STATE ".tmp","w"); if (!f) return -1;
  int ok=fprintf(f,"%d\n",v)>0 && fflush(f)==0 && fsync(fileno(f))==0;
  if (fclose(f)!=0) ok=0;
  if (ok && rename(STATE ".tmp",STATE)==0) return 0;
  unlink(STATE ".tmp"); return -1;
}
void control_notify(const char *msg) {
  notify_request req={0}; snprintf(req.message,sizeof(req.message),"%s",msg);
  sceKernelSendNotificationRequest(0,&req,sizeof(req),0);
}
static void notify_volume(int v) {
  notify_request req={0};
  if (v&TTGK_MUTE) snprintf(req.message,sizeof(req.message),"TTGK USB Audio: muted");
  else snprintf(req.message,sizeof(req.message),"TTGK USB Audio: %.1f dB",(v&255)*0.5-74.0);
  sceKernelSendNotificationRequest(0,&req,sizeof(req),0);
}
static void *pad_run(void *arg) {
  (void)arg;
  int pad_ok=0, pad_attempted=access(DIR "/disable_pad",F_OK)==0;
  pad_report(pad_attempted?1:0,0);
  if (pad_attempted) log_line("TTGK: DualSense disabled by file");
  int handle=-1,owned=0,user=-1;
  uint64_t retry=0,user_check=0;
  hotkey_t h={0}; int released=0;
  while (!atomic_load(&quitting)) {
    uint64_t t=ms();
    if (!pad_attempted && atomic_load(&pad_ready)) {
      pad_attempted=1; pad_ok=pad_init()==0;
      if (!pad_ok) log_line("TTGK: direct DualSense API unavailable; use volume.request");
    }
    if (pad_ok && t>=user_check) {
      user_check=t+1000;
      int new_user=-1; int ur=get_user(&new_user);
      if (ur<0) new_user=-1;
      atomic_store(&pad_user,new_user);
      if (new_user==-1 || new_user==0) pad_report(6,ur);
      if (new_user!=user) {
        if (handle>=0 && owned) close_pad(handle);
        handle=-1; owned=0; user=new_user; retry=0; h=(hotkey_t){0}; released=0;
        atomic_store(&pad_handle,-1); atomic_store(&pad_connected,0); atomic_store(&pad_buttons,0);
      }
    }
    if (pad_ok && handle<0 && user!=-1 && user!=0 && t>=retry) {
      retry=t+5000; handle=get_handle(user,0,0);
      if (handle<0) { handle=open_pad(user,0,0,NULL); owned=handle>=0; }
      atomic_store(&pad_handle,handle);
      if (handle<0) { pad_report(7,handle); log_line("TTGK: pad handle unavailable %#x",(unsigned)handle); }
      else { pad_report(8,0); log_line("TTGK: pad handle ready"); }
    }
    if (!(atomic_load(&desired)&ONLINE)) { h=(hotkey_t){0}; released=0; }
    if (handle>=0) {
      union { pad_data d; uint64_t align; uint8_t storage[512]; } state={0};
      int r=read_pad(handle,&state);
      atomic_fetch_add(&pad_reads,1);
      atomic_store(&pad_last_ms,t);
      if (r!=0 || !state.d.connected) {
        pad_report(r!=0?9:10,r);
        atomic_store(&pad_connected,0); atomic_store(&pad_buttons,0);
        if (owned) close_pad(handle);
        handle=-1; owned=0; retry=t+1000; h=(hotkey_t){0}; released=0;
        atomic_store(&pad_handle,-1);
      } else {
        uint32_t b=state.d.buttons;
        atomic_store(&pad_connected,1);
        pad_report((b&0x80000000u)?11:12,0);
        uint32_t old=atomic_exchange(&pad_buttons,b);
        if (old!=b) {
          atomic_fetch_add(&pad_changes,1);
          uint32_t pressed=b&~old&0x7fffffffu;
          if (pressed) atomic_store(&pad_last_pressed,pressed);
          if (t<atomic_load(&pad_trace_until))
            log_line("pad trace: buttons=%08x pressed=%08x phase=%d",b,pressed,h.phase);
        }
        if (b&0x80000000u) { h=(hotkey_t){0}; released=0; }
        else {
          if ((b&PAD_MOD)!=PAD_MOD) released=1;
          int state=atomic_load(&desired);
          if (released && (state&ONLINE)) {
            int v=hotkey(&h,b,t,state&511);
            if (v!=(state&511)) request_volume(v);
          }
        }
      }
      atomic_store(&pad_phase,h.phase);
    }
    usleep(40000);
  }
  if (handle>=0 && owned) close_pad(handle);
  return NULL;
}
static void *control_worker(void *arg) {
  (void)arg;
  int last=-1,persisted=-1,shown=-1,current;
  if (read_value(STATE,&persisted)<0) persisted=-1;
  uint64_t changed=0,notify_at=0,files_at=0;
  while (!atomic_load(&quitting)) {
    uint64_t t=ms();
    if (t>=files_at) {
      files_at=t+250;
      if (unlink(STOP)==0) { atomic_store(&quitting,1); break; }
      if (rename(REQUEST,REQUEST ".processing")==0) {
        if (read_value(REQUEST ".processing",&current)==0) {
          if (atomic_load(&desired)&ONLINE) request_volume(current);
          else log_line("TTGK: volume.request ignored (DAC disconnected)");
        } else log_line("TTGK: invalid volume.request");
        unlink(REQUEST ".processing");
      }
    }
    int a=atomic_load(&applied);
    if (a>=0 && a!=last) { last=a; changed=t; }
    if (last>=0 && last!=persisted && t-changed>=1000) {
      if (save_value(last)==0) persisted=last;
      else { changed=t; log_line("TTGK: cannot save volume"); }
    }
    /* Show the last value after a held button, even if it landed in cooldown. */
    if (last>=0 && shown!=last && t>=notify_at) {
      notify_volume(last); shown=last; notify_at=t+600;
    }
    usleep(100000);
  }
  int a=atomic_load(&applied);
  if (a>=0 && a!=persisted) save_value(a);
  return NULL;
}
int control_start(void) {
  int v;
  if (read_value(MAX_FILE,&v)==0 && !(v&TTGK_MUTE)) max_volume=v;
  if (read_value(STATE,&v)==0) atomic_store(&desired,clamp_volume(v));
  else atomic_store(&desired,clamp_volume(TTGK_DEFAULT));
  int r=pthread_create(&worker,NULL,control_worker,NULL);
  if (r) { log_line("TTGK: control thread failed (%d)",r); return -1; }
  started=1;
  if (access(DIR "/disable_pad",F_OK)!=0) {
    if (!pthread_create(&pad_worker,NULL,pad_run,NULL)) pad_started=1;
    else log_line("TTGK: pad worker failed; file controls remain available");
  } else { pad_report(1,0); log_line("TTGK: DualSense disabled by file"); }
  return 0;
}
int control_set(int v) { return volume_valid(v)?request_volume(v):0; }
int control_adjust(int delta,int toggle) {
  int old=atomic_load(&desired);
  while (old&ONLINE) {
    int v=old&511;
    int n=ONLINE|clamp_volume(toggle?(v^TTGK_MUTE):volume_step(v,delta));
    if (atomic_compare_exchange_weak(&desired,&old,n)) return 1;
  }
  return 0;
}
int control_limit(void) { return max_volume; }
int control_verified(void) { return atomic_load(&applied); }
int control_online(void) { return !!(atomic_load(&desired)&ONLINE); }
void control_request_stop(void) { atomic_store(&quitting,1); }
void control_pad_trace(int on) { atomic_store(&pad_trace_until,on?ms()+60000:0); }
void control_pad_snapshot(pad_snapshot *s) {
  *s=(pad_snapshot){atomic_load(&pad_stage),atomic_load(&pad_err),atomic_load(&pad_handle),atomic_load(&pad_user),atomic_load(&pad_connected),atomic_load(&pad_phase),atomic_load(&pad_buttons),atomic_load(&pad_last_pressed),atomic_load(&pad_changes),atomic_load(&pad_reads),atomic_load(&pad_last_ms)};
}
void control_enable_pad(void) { atomic_store(&pad_ready,1); }
int control_volume(void) { return atomic_load(&desired)&511; }
void control_connected(void) { atomic_fetch_or(&desired,ONLINE); }
void control_disconnected(int failed) {
  int last=atomic_load(&applied);
  if (last<0) last=control_volume();
  if (failed) last|=TTGK_MUTE;
  atomic_store(&desired,clamp_volume(last));
}
void control_applied(int v) { atomic_store(&applied,v); }
int control_stop_requested(void) { return atomic_load(&quitting); }
void control_stop(void) {
  atomic_store(&quitting,1);
  if (started) pthread_join(worker,NULL);
  if (pad_started) pthread_join(pad_worker,NULL);
}
