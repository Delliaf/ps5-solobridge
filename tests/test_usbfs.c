/* Test real USB_FS wrapper with mocked kernel ioctls, not hardware. */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include "../solobridge/usbfs.h"
#include "../solobridge/bench.h"
static fs_endpoint *ep;
static unsigned frames,opened,starts,uninit,slot;
static int fail_open,short_frame,bad_status,empty;
void log_line(const char *fmt,...){(void)fmt;}
int ioctl(int fd,unsigned long cmd,...){
  (void)fd;va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void*);va_end(ap);
  if(cmd==FS_INIT){fs_init *x=arg;assert(x->ep_index_max==FS_SLOTS);ep=x->pEndpoints;opened=0;return 0;}
  if(cmd==FS_OPEN){fs_open *x=arg;assert(x->ep_no==1&&x->ep_index==opened);if(fail_open&&opened==1){errno=ENOTTY;return -1;}frames=x->max_frames;x->max_packet_length=288;opened++;return 0;}
  if(cmd==FS_START){fs_index *x=arg;slot=x->ep_index;assert(slot<opened&&ep[slot].nFrames==frames);for(unsigned i=0;i<frames;i++){assert(ep[slot].ppBuffer[i]&&ep[slot].pLength[i]==36);}starts++;return 0;}
  if(cmd==FS_COMPLETE){if(empty){errno=EBUSY;return -1;}fs_index *x=arg;x->ep_index=slot;ep[slot].status=bad_status;ep[slot].aFrames=frames;if(short_frame)ep[slot].pLength[0]=35;return 0;}
  if(cmd==FS_UNINIT){uninit++;return 0;}
  assert(0);return -1;
}
int main(void){
  usbfs f;
  for(unsigned n=16;n<=64;n*=2){
    assert(fs_setup(&f,1,n)==0&&f.opened==3);
    assert(fs_submit(&f,1,1)==0&&f.submitted==n);
    assert(fs_submit(&f,1,1)<0);
    empty=1;assert(fs_complete(&f,1)==0);empty=0;
    assert(fs_complete(&f,1)==2&&f.completed==n);
    assert(fs_submit(&f,1,2)==0);short_frame=1;assert(fs_complete(&f,1)<0);short_frame=0;
    assert(fs_submit(&f,1,0)==0);bad_status=1;assert(fs_complete(&f,1)<0);bad_status=0;
    assert(fs_cleanup(&f,1)==0);
  }
  fail_open=1;assert(fs_setup(&f,1,32)<0&&f.initialized);assert(fs_cleanup(&f,1)==0&&uninit==4);
  bench_sample a={.cpu={.at=1}},b={.cpu={.at=26},.packets=200000};
  assert(bench_pass(a,b,0)&&bench_pass(a,b,64));
  b.underruns=1;assert(!bench_pass(a,b,32));b.underruns=0;b.packets=190000;assert(!bench_pass(a,b,32));
  puts("USB_FS ABI, completion, partial-open cleanup and benchmark counter tests passed");
}
