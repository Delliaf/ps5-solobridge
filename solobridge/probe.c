/* GPL-3.0. Explicit opt-in local source-mix diagnostic.
 * Captures only what the bridge already receives from AVCAP; never opens a mic.
 * The capture thread copies PCM to bounded RAM, writer thread owns disk I/O. */
#include "probe.h"
#include "probe_logic.h"
#include "log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef PROBE_ROOT
#define PROBE_ROOT "/data/solobridge"
#endif
#define ROOT PROBE_ROOT
#define OUT ROOT "/tempest"
#define REQUEST ROOT "/tempest_probe.request"
#define ENABLE ROOT "/enable_tempest_probe"
enum { IDLE,ARMED,RECORDING,DONE,FAILED };
static atomic_int enabled,quit,state,cancel;
static pthread_t worker;
static uint8_t *pcm;
static size_t target,used;
static char label[32];
/* Host tests shorten the timeout only, not the sample count. */
#ifndef PROBE_TIMEOUT_EXTRA
#define PROBE_TIMEOUT_EXTRA 8
#endif
static double secs(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec/1e9;
}
static int read_label(char out[32]) {
  FILE *fp=fopen(REQUEST ".processing","rb");
  if (!fp) return 0;
  size_t n=fread(out,1,31,fp);
  int ok=feof(fp)&&!ferror(fp)&&n&&!memchr(out,0,n);
  fclose(fp);
  if (!ok) return 0;
  while(n && (out[n-1]=='\r'||out[n-1]=='\n')) n--;
  out[n]=0;
  return probe_duration(out)!=0;
}
static void status(const char *msg) {
  FILE *fp=fopen(OUT "/status.tmp","w");
  if (!fp) return;
  int ok=fprintf(fp,"%s\n",msg)>0;
  if (fclose(fp)!=0) ok=0;
  if (ok) rename(OUT "/status.tmp",OUT "/status.txt");
  else unlink(OUT "/status.tmp");
}
static int write_wav(void) {
  char path[128],tmp[128];
  snprintf(path,sizeof(path),OUT "/%s.wav",label);
  snprintf(tmp,sizeof(tmp),OUT "/%s.wav.tmp",label);
  uint8_t h[44]; probe_wav_header(h,(uint32_t)target);
  FILE *fp=fopen(tmp,"wb");
  if (!fp) return -1;
  int ok=fwrite(h,1,sizeof(h),fp)==sizeof(h) &&
    fwrite(pcm,4,target,fp)==target && fflush(fp)==0 && fsync(fileno(fp))==0;
  if (fclose(fp)!=0) ok=0;
  if (ok && rename(tmp,path)==0) return 0;
  unlink(tmp); return -1;
}
static void *run(void *arg) {
  (void)arg;
  status("idle; submit one request when ready");
  double deadline=0; int timed_out=0;
  while (!atomic_load(&quit)) {
    int s=atomic_load(&state);
    if (s==IDLE) {
      if (rename(REQUEST,REQUEST ".processing")==0) {
        char name[32];
        if (read_label(name)) {
          snprintf(label,sizeof(label),"%s",name);
          target=(size_t)probe_duration(name)*PROBE_RATE;
          used=0; atomic_store(&cancel,0);
          deadline=secs()+probe_duration(name)+PROBE_TIMEOUT_EXTRA;
          timed_out=0;
          /* Publish only after all request fields initialized. */
          atomic_store(&state,ARMED);
          status("armed; waiting for source PCM");
          log_line("Tempest probe: %s armed (%u sec)",label,probe_duration(label));
        } else status("invalid request; see docs/TV_TEMPEST.md");
        unlink(REQUEST ".processing");
      }
    } else if (s==ARMED || s==RECORDING) {
      if (!timed_out && secs()>deadline) {
        timed_out=1;
        atomic_store(&cancel,1);
        int expected=ARMED;
        atomic_compare_exchange_strong(&state,&expected,FAILED);
        status("timeout; waiting for capture worker to acknowledge");
      }
    } else if (s==DONE) {
      if (write_wav()==0) {
        char msg[100]; snprintf(msg,sizeof(msg),"complete: %s.wav",label);
        status(msg); log_line("Tempest probe: %s saved",label);
      } else { status("failed: WAV write"); log_line("Tempest probe: write failed"); }
      atomic_store(&state,IDLE);
    } else if (s==FAILED) {
      status("failed: capture interrupted or timed out; repeat");
      log_line("Tempest probe: %s interrupted; no WAV committed",label);
      atomic_store(&state,IDLE);
    }
    usleep(100000);
  }
  return NULL;
}
int probe_start(void) {
  if (access(ENABLE,F_OK)!=0) return 0;
  if (mkdir(OUT,0700)<0 && access(OUT,W_OK)!=0) return -1;
  pcm=malloc((size_t)PROBE_MAX_SECONDS*PROBE_RATE*4);
  if (!pcm) return -1;
  atomic_store(&state,IDLE); atomic_store(&quit,0); atomic_store(&cancel,0);
  /* A previous request is never executed after a reboot. */
  unlink(REQUEST); unlink(REQUEST ".processing");
  if (pthread_create(&worker,NULL,run,NULL)) { free(pcm); pcm=NULL; return -1; }
  atomic_store(&enabled,1); return 1;
}
/* Only called by source thread. No allocation, locks, syscalls or disk writes. */
void probe_feed(const float *src,size_t frames) {
  if (!atomic_load(&enabled)) return;
  int s=atomic_load(&state);
  if (s==ARMED) {
    int expected=ARMED;
    if (!atomic_compare_exchange_strong(&state,&expected,RECORDING)) return;
    s=RECORDING;
  }
  if (s!=RECORDING) return;
  if (atomic_load(&cancel)) { atomic_store(&state,FAILED); return; }
  size_t n=target-used; if (frames<n) n=frames;
  probe_pcm(pcm+used*4,src,n); used+=n;
  if (used==target) atomic_store(&state,atomic_load(&cancel)?FAILED:DONE);
}
void probe_discontinuity(void) {
  if (!atomic_load(&enabled)) return;
  int s=atomic_load(&state);
  if (s==ARMED || s==RECORDING) {
    atomic_compare_exchange_strong(&state,&s,FAILED);
  }
}
/* Called after source_stop (no remaining producers). */
void probe_stop(void) {
  if (!atomic_exchange(&enabled,0)) return;
  atomic_store(&quit,1); pthread_join(worker,NULL); free(pcm); pcm=NULL;
}
