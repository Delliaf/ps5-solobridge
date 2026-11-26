/* GPL-3.0. Local controller, persistence and file fallback.
 * API references: PS5 SDK sce_stubs and octet/src/ps5_vpad.c.
 * No injection, privilege changes or synthetic input. */
#include "control.h"
#include "bridge_logic.h"
#include "log.h"
#include "version.h"
#include <semaphore.h>
#include <stddef.h>
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
#define MAX_FILE DIR "/max_volume"
static int max_volume=136; /* -6 dB by default; 148 restores full range */
static atomic_int desired=TTGK_DEFAULT,applied=-1,quitting=0;
static int clamp_volume(int v) { return (v&255)>max_volume?(v&TTGK_MUTE)|max_volume:v; }
static int request_volume(int v) {
  int old=atomic_load(&desired);
  while (old&ONLINE) {
    int n=ONLINE|clamp_volume(v);
    if (atomic_compare_exchange_weak(&desired,&old,n)) return 1;
  }
  return 0;
}
static pthread_t worker,notify_worker;
static int started;
static atomic_int notify_started;
static sem_t notify_event;
static atomic_int notify_pending,notify_result=INT32_MIN,notify_quit;
typedef struct { int32_t type,req_id,priority,msg_id,target_id,user_id,unk1,unk2,app_id,error_num,unk3; uint8_t use_icon; char message[1024],uri[1024],extra[1024]; } notify_request;
_Static_assert(sizeof(notify_request)==3120,"notification ABI");
_Static_assert(offsetof(notify_request,message)==45,"notification message ABI");
int sceKernelSendNotificationRequest(int,notify_request *,size_t,int);
static uint64_t ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void send_notification(int v) {
  notify_request req={0};
  req.target_id=-1;req.use_icon=1;
  snprintf(req.uri,sizeof(req.uri),"cxml://psnotification/tex_icon_system");
  if(v<0)snprintf(req.message,sizeof(req.message),"TTGK Audio Bridge v%s loaded",TTGK_VERSION);
  else if(v&TTGK_MUTE)snprintf(req.message,sizeof(req.message),"TTGK v%s: Mute",TTGK_VERSION);
  else snprintf(req.message,sizeof(req.message),"TTGK v%s: %.1f dB",TTGK_VERSION,(v&255)*0.5-74.0);
  /* Full etaHEN-style request first; ELF loader-style zero header only
   * if the API explicitly rejects it. A zero return is not visual proof. */
  int r=sceKernelSendNotificationRequest(0,&req,sizeof(req),0);
  log_line("notification v%s full result=%#x",TTGK_VERSION,(unsigned)r);
  if(r<0) {
    req.target_id=0;req.use_icon=0;memset(req.uri,0,sizeof(req.uri));
    r=sceKernelSendNotificationRequest(0,&req,sizeof(req),0);
    log_line("notification v%s fallback result=%#x",TTGK_VERSION,(unsigned)r);
  }
  atomic_store(&notify_result,r);
}
static void *notify_run(void *arg) {
  (void)arg;
  for(;;) {
    if(sem_wait(&notify_event)<0){if(errno==EINTR)continue;break;}
    if(atomic_load(&notify_quit))break;
    int v=atomic_exchange(&notify_pending,0);
    if(v)send_notification(v-2);
  }
  return NULL;
}
static int queue_notification(int v) {
  if(!notify_started)return 0;
  if(!atomic_exchange(&notify_pending,v+2))sem_post(&notify_event);
  return 1;
}
int control_test_notification(void){return queue_notification(-1);}
int control_notification_result(void){return atomic_load(&notify_result);}
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
static void *control_worker(void *arg) {
  (void)arg;
  int last=-1,persisted=-1,shown=-1,current;
  if (read_value(STATE,&persisted)<0) persisted=-1;
  uint64_t changed=0,files_at=0;
  int notifications=access(DIR "/enable_notifications",F_OK)==0;
  while (!atomic_load(&quitting)) {
    uint64_t t=ms();
    if (t>=files_at) {
      files_at=t+1000;
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
    if (notifications && last>=0 && shown!=last && t-changed>=1000) {
      if(shown>=0)queue_notification(last);
      shown=last;
    }
    usleep(250000);
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
  if(sem_init(&notify_event,0,0)==0) {
    if(!pthread_create(&notify_worker,NULL,notify_run,NULL)) {
      notify_started=1;queue_notification(-1);
    } else sem_destroy(&notify_event);
  }
  if(!notify_started)log_line("notification worker unavailable");
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
  if(notify_started){atomic_store(&notify_quit,1);sem_post(&notify_event);pthread_join(notify_worker,NULL);sem_destroy(&notify_event);}
}
