/* TTGK 3302:33c0 UAC2 synchronous 48kHz packed 24-bit. GPL-3.0. */
#include "solo.h"
#include "bridge_logic.h"
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
  return 0;
}
int solo_set_volume(solo_t *s,int volume) {
  if (!volume_valid(volume)) { errno=EINVAL; return -1; }
  uint16_t value=(uint16_t)volume_units(volume);
  if ((volume&TTGK_MUTE) && set_mute(s,1)<0) goto fail;
  for (int c=1;c<=2;c++) {
    uint8_t d[2]={(uint8_t)value,(uint8_t)(value>>8)},got[2]={0};
    if (request(s,1,2,2,c,d,2)<0 || request(s,0,2,2,c,got,2)<0) goto fail;
    if (memcmp(d,got,2)) { errno=EIO; goto fail; }
  }
  if (!(volume&TTGK_MUTE) && set_mute(s,0)<0) goto fail;
  return 0;
fail: {
    int saved=errno; set_mute(s,1); errno=saved; return -1;
  }
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
  if ((rate_set<0 && clock_rate(s,1)<0)||clock_rate(s,0)<0) goto fail;
  char path[40]; snprintf(path,sizeof(path),"/dev/usb/%d.%d.1",s->bus,s->addr);
  what=path; s->ep_fd=open(path,O_WRONLY|O_NONBLOCK);
  if (s->ep_fd<0) goto fail;
  what="initial hardware volume"; if (solo_set_volume(s,volume)<0) goto fail;
  if (verbose) printf("TTGK ugen%d.%d: 48000 Hz stereo packed 24-bit ep 01\n",s->bus,s->addr);
  return 0;
fail:
  fprintf(stderr,"TTGK: %s failed (errno %d)\n",what,errno);
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
  if (s->ep_fd>=0) { close(s->ep_fd); s->ep_fd=-1; }
  if (s->ctl_fd>=0) {
    if (s->configured) { set_mute(s,1); set_alt(s,0); }
    close(s->ctl_fd); s->ctl_fd=-1;
  }
  s->configured=0;
}
