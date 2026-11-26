/* TTGK 3302:33c0 UAC2 synchronous 48kHz packed 24-bit. GPL-3.0. */
#include "solo.h"
#include "bridge_logic.h"
#include "log.h"
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
static int find_solo(solo_t *s) {
  DIR *dir=opendir("/dev");
  if (!dir) return -1;
  struct dirent *entry; int result=-1;
  while ((entry=readdir(dir))) {
    int bus,addr; char tail;
    if (sscanf(entry->d_name,"ugen%d.%d%c",&bus,&addr,&tail)!=2 ||
        bus<0 || bus>255 || addr<2 || addr>127) continue;
    char path[40]; snprintf(path,sizeof(path),"/dev/ugen%d.%d",bus,addr);
    int fd=open(path,O_RDWR);
    if (fd<0) continue;
    struct usb_device_descriptor dd;
    if (ioctl(fd,USB_GET_DEVICE_DESC,&dd)==0 &&
        UGETW(dd.idVendor)==0x3302 && UGETW(dd.idProduct)==0x33c0) {
      s->bus=bus; s->addr=addr; result=fd; break;
    }
    close(fd);
  }
  closedir(dir); return result;
}
static int set_alt(solo_t *s,int alt) {
  struct usb_alt_interface ai={.uai_interface_index=1,.uai_alt_index=alt};
  return ioctl(s->ctl_fd,USB_SET_ALTINTERFACE,&ai);
}
static int request(solo_t *s,int write,int entity,int selector,int channel,
                   void *data,uint16_t len) {
  struct usb_ctl_request r;
  memset(&r,0,sizeof(r)); r.ucr_data=data;
  r.ucr_request.bmRequestType=(write?UT_WRITE:UT_READ)|UT_CLASS|UT_INTERFACE;
  r.ucr_request.bRequest=1;
  USETW(r.ucr_request.wValue,(selector<<8)|channel);
  USETW(r.ucr_request.wIndex,entity<<8);
  USETW(r.ucr_request.wLength,len);
  if (ioctl(s->ctl_fd,USB_DO_REQUEST,&r)<0) return -1;
  if (!write && r.ucr_actlen!=len) { errno=EIO; return -1; }
  return 0;
}
static int set_mute(solo_t *s,int mute) {
  uint8_t v=(uint8_t)!!mute,got=0xff;
  if (request(s,1,2,1,0,&v,1)<0 || request(s,0,2,1,0,&got,1)<0) return -1;
  if (got!=v) { errno=EIO; return -1; }
  s->hw.mute=mute;
  return 0;
}
/* Only the control worker touches the control fd during streaming.
 * Startup/shutdown own it only before/after join. */
#define HOLD_MUTE 512
static uint64_t millis(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int hw_mute(void *ctx,int mute) { return set_mute(ctx,mute); }
static int hw_channel(void *ctx,int channel,int value) {
  solo_t *s=ctx; uint16_t u=(uint16_t)value;
  uint8_t d[2]={(uint8_t)u,(uint8_t)(u>>8)},got[2]={0};
  if (request(s,1,2,2,channel,d,2)<0 || request(s,0,2,2,channel,got,2)<0) return -1;
  if (memcmp(d,got,2)) { errno=EIO; return -1; }
  return 0;
}
static int allow_unmute(void *ctx) {
  solo_t *s=ctx;
  return !atomic_load(&s->volume_stop) &&
    atomic_load(&s->volume_wanted)==s->transaction;
}
static const hw_volume_ops ops={hw_mute,hw_channel,allow_unmute};
static int apply_volume(solo_t *s,int volume) {
  if (!volume_valid(volume)) { errno=EINVAL; return -1; }
  return hw_volume_apply(&s->hw,&ops,s,volume);
}
static void *volume_run(void *arg) {
  solo_t *s=arg; int last=-1;
  while (!atomic_load(&s->volume_stop)) {
    int v=atomic_load(&s->volume_wanted);
    if (v!=last) {
      s->transaction=v;
      int physical=(v&511)|((v&HOLD_MUTE)?TTGK_MUTE:0);
      atomic_store(&s->volume_busy_ms,millis());
      int r=apply_volume(s,physical);
      atomic_store(&s->volume_busy_ms,0);
      if (r<0) {
        int e=errno;
        atomic_store(&s->volume_error,1); /* before best-effort mute */
        log_line("USB: volume control failed errno=%d; closing stream",e);
        set_mute(s,1); break;
      }
      if (!r) { last=v; atomic_store(&s->volume_done,v); }
      else { last=-1; atomic_store(&s->volume_done,-1); }
    }
    if (atomic_load(&s->volume_stop)) break;
    if (atomic_load(&s->volume_wanted)!=last) continue;
    if (atomic_load(&s->event_ok)) {
      /* One post per changed request; periodic timeout is a safety fallback. */
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME,&deadline);
      deadline.tv_nsec+=100000000;
      if (deadline.tv_nsec>=1000000000) { deadline.tv_nsec-=1000000000; deadline.tv_sec++; }
      if (sem_timedwait(&s->volume_event,&deadline)<0 && errno!=ETIMEDOUT && errno!=EINTR)
        atomic_store(&s->event_ok,0);
    } else usleep(10000);
  }
  return NULL;
}
void solo_request_volume(solo_t *s,int volume,int ready) {
  int v=volume|(ready?0:HOLD_MUTE);
  if (atomic_exchange(&s->volume_wanted,v)!=v && atomic_load(&s->event_ok))
    if (sem_post(&s->volume_event)<0) atomic_store(&s->event_ok,0);
}
int solo_volume_done(solo_t *s) {
  int v=atomic_load(&s->volume_done);
  return v<0 || (v&HOLD_MUTE)?-1:v;
}
int solo_volume_error(solo_t *s) {
  uint64_t busy=atomic_load(&s->volume_busy_ms);
  return atomic_load(&s->volume_error) || (busy && millis()-busy>500);
}
static int clock_valid(solo_t *s) {
  /* Clock Source 9 advertises read access to Clock Validity. */
  for (int i=0;i<20;i++) {
    uint8_t valid=0;
    if (request(s,0,9,2,0,&valid,1)<0) return -1;
    if (valid==1) return 0;
    usleep(10000);
  }
  errno=ETIMEDOUT; return -1;
}
static int clock_rate(solo_t *s,int write) {
  uint8_t rate[4]={0x80,0xbb,0,0};
  if (!write) memset(rate,0,sizeof(rate));
  if (request(s,write,9,1,0,rate,4)<0) return -1;
  if (!write) {
    s->rate=(uint32_t)rate[0]|((uint32_t)rate[1]<<8)|((uint32_t)rate[2]<<16)|((uint32_t)rate[3]<<24);
    if (s->rate!=SOLO_RATE) { errno=EIO; return -1; }
  }
  return 0;
}
int solo_open(solo_t *s,int verbose,int volume) {
  memset(s,0,sizeof(*s)); s->ctl_fd=s->ep_fd=-1;
  s->hw=(hw_volume_state){-1,-1};
  atomic_init(&s->volume_wanted,volume|HOLD_MUTE); atomic_init(&s->volume_done,-1);
  atomic_init(&s->volume_stop,0); atomic_init(&s->volume_error,0); atomic_init(&s->volume_busy_ms,0); atomic_init(&s->event_ok,0);
  s->ctl_fd=find_solo(s);
  if (s->ctl_fd<0) return -1;
  uint8_t buf[4096]={0};
  struct usb_gen_descriptor gd;
  memset(&gd,0,sizeof(gd)); gd.ugd_data=buf; gd.ugd_maxlen=sizeof(buf);
  const char *what="USB descriptor";
  if (ioctl(s->ctl_fd,USB_GET_FULL_DESC,&gd)<0) goto fail;
  size_t n=gd.ugd_actlen?gd.ugd_actlen:(size_t)(buf[2]|(buf[3]<<8));
  if (n>sizeof(buf)||!ttgk_config(buf,n)) { errno=EINVAL; goto fail; }
  s->maxpkt=288; s->channels=2; s->subslot=3; s->bits=24; s->configured=1;
  what="initial mute"; if (set_mute(s,1)<0) goto fail;
  what="alt 0"; if (set_alt(s,0)<0) goto fail;
  int rate_set=clock_rate(s,1);
  what="alt 2"; if (set_alt(s,2)<0) goto fail;
  what="48000 Hz clock";
  if (rate_set<0 || clock_rate(s,0)<0) {
    if (clock_rate(s,1)<0 || clock_rate(s,0)<0) goto fail;
  }
  what="clock validity"; if (clock_valid(s)<0) goto fail;
  char path[40]; snprintf(path,sizeof(path),"/dev/usb/%d.%d.1",s->bus,s->addr);
  what=path; s->ep_fd=open(path,O_WRONLY|O_NONBLOCK);
  if (s->ep_fd<0) goto fail;
  what="initial hardware volume"; if (apply_volume(s,volume|TTGK_MUTE)<0) goto fail;
  if (sem_init(&s->volume_event,0,0)==0) { s->sem_initialized=1; atomic_store(&s->event_ok,1); }
  what="volume thread"; int err=pthread_create(&s->volume_thread,NULL,volume_run,s);
  if (err) { errno=err; goto fail; }
  s->worker_started=1;
  if (verbose) log_line("TTGK ugen%d.%d: 48000 Hz stereo packed 24-bit ep 01",s->bus,s->addr);
  return 0;
fail:
  log_line("TTGK: %s failed (errno %d)",what,errno);
  solo_close(s); return -1;
}
int solo_write(solo_t *s,const void *pkt,size_t len) {
  if (len!=36 || len>s->maxpkt) { errno=EINVAL; return -1; }
  ssize_t w=write(s->ep_fd,pkt,len);
  if (w==(ssize_t)len) return 1;
  if (w<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) return 0;
  if (w>=0) errno=EIO;
  return -1;
}
void solo_close(solo_t *s) {
  if (s->worker_started) {
    atomic_store(&s->volume_stop,1); atomic_fetch_or(&s->volume_wanted,HOLD_MUTE);
    if (s->sem_initialized) sem_post(&s->volume_event);
  }
  if (s->ep_fd>=0) { close(s->ep_fd); s->ep_fd=-1; }
  if (s->worker_started) { pthread_join(s->volume_thread,NULL); s->worker_started=0; }
  if (s->sem_initialized) { sem_destroy(&s->volume_event); s->sem_initialized=0; }
  if (s->ctl_fd>=0 && s->configured) set_mute(s,1);
  if (s->ctl_fd>=0) {
    if (s->configured) set_alt(s,0);
    close(s->ctl_fd); s->ctl_fd=-1;
  }
  s->configured=0;
}
