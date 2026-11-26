#include "usbfs.h"
#include "log.h"
#include <errno.h>
#include <string.h>
int fs_setup(usbfs *f,int fd,unsigned n) {
  memset(f,0,sizeof(*f));
  if(n!=16&&n!=32&&n!=64){errno=EINVAL;return -1;}
  f->n=n;
  fs_init init={.pEndpoints=f->ep,.ep_index_max=FS_SLOTS};
  if(ioctl(fd,FS_INIT,&init)<0){log_line("USB_FS INIT errno=%d",errno);return -1;}
  f->initialized=1;
  for(unsigned i=0;i<FS_SLOTS;i++){
    fs_open o={.max_bufsize=n*288,.max_frames=n,.ep_index=i,.ep_no=1};
    if(ioctl(fd,FS_OPEN,&o)<0){log_line("USB_FS OPEN slot=%u errno=%d",i,errno);return -1;}
    f->opened++;
    log_line("USB_FS OPEN slot=%u frames=%u max_packet=%u bytes=%u",i,o.max_frames,o.max_packet_length,o.max_bufsize);
    if(o.max_frames<n||o.max_packet_length<FS_BYTES||o.max_bufsize<n*FS_BYTES){errno=EINVAL;return -1;}
    f->ep[i].ppBuffer=f->ptr[i];f->ep[i].pLength=f->len[i];
    for(unsigned j=0;j<n;j++)f->ptr[i][j]=f->data[i][j];
  }
  return 0;
}
int fs_submit(usbfs *f,int fd,unsigned i){
  if(i>=f->opened||(f->busy&(1u<<i))){errno=EINVAL;return -1;}
  for(unsigned j=0;j<f->n;j++)f->len[i][j]=FS_BYTES;
  f->ep[i].nFrames=f->n;f->ep[i].aFrames=0;f->ep[i].status=-1;
  f->ep[i].flags=0;f->ep[i].timeout=100;
  fs_index x={.ep_index=i};f->calls++;
  if(ioctl(fd,FS_START,&x)<0)return -1;
  f->busy|=1u<<i;f->submitted+=f->n;return 0;
}
/* Returns completed slot + 1, 0 if no completion, -1 if invalid/short. */
int fs_complete(usbfs *f,int fd){
  fs_index x={0};f->calls++;
  if(ioctl(fd,FS_COMPLETE,&x)<0){
    if(errno==EBUSY||errno==EAGAIN||errno==EINTR)return 0;
    return -1;
  }
  unsigned i=x.ep_index;
  if(i>=f->opened||!(f->busy&(1u<<i))){errno=EIO;return -1;}
  f->busy&=~(1u<<i);
  if(f->ep[i].status||f->ep[i].aFrames!=f->n){
    log_line("USB_FS completion status=%d frames=%u expected=%u",f->ep[i].status,f->ep[i].aFrames,f->n);errno=EIO;return -1;
  }
  for(unsigned j=0;j<f->n;j++)if(f->len[i][j]!=FS_BYTES){
    log_line("USB_FS short frame slot=%u frame=%u len=%u",i,j,f->len[i][j]);errno=EIO;return -1;
  }
  f->completed+=f->n;return (int)i+1;
}
int fs_cleanup(usbfs *f,int fd){
  if(!f->initialized)return 0;
  /* UNINIT cancels and drains every opened transfer, including partial setup. */
  fs_index x={0};
  int r=ioctl(fd,FS_UNINIT,&x);
  if(r<0)log_line("USB_FS UNINIT errno=%d; closing device",errno);
  f->initialized=0;return r;
}
