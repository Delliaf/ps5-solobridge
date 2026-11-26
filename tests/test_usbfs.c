/* Test real USB_FS wrapper with mocked kernel ioctls, not hardware. */
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include "../solobridge/usbfs.h"
#include "../solobridge/bench.h"
static fs_endpoint *ep;
static unsigned frames,opened,starts,uninit,slot,expected_slots=3;
static int fail_open,short_frame,bad_status,empty,drain_mode,drain_pending;
void log_line(const char *fmt,...){(void)fmt;}
int ioctl(int fd,unsigned long cmd,...){
  (void)fd;va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void*);va_end(ap);
  if(cmd==FS_INIT){fs_init *x=arg;assert(x->ep_index_max==expected_slots);ep=x->pEndpoints;opened=0;return 0;}
  if(cmd==FS_OPEN){fs_open *x=arg;assert(x->ep_no==1&&x->ep_index==opened);if(fail_open&&opened==1){errno=ENOTTY;return -1;}frames=x->max_frames;x->max_packet_length=288;opened++;return 0;}
  if(cmd==FS_START){fs_index *x=arg;slot=x->ep_index;assert(slot<opened&&ep[slot].nFrames==frames);for(unsigned i=0;i<frames;i++){assert(ep[slot].ppBuffer[i]&&ep[slot].pLength[i]==36);}starts++;return 0;}
  if(cmd==FS_COMPLETE){if(drain_mode&&drain_pending==0){errno=EBUSY;return -1;}if(drain_mode)drain_pending--;if(empty){errno=EBUSY;return -1;}fs_index *x=arg;x->ep_index=slot;ep[slot].status=bad_status;ep[slot].aFrames=frames;if(short_frame)ep[slot].pLength[0]=35;return 0;}
  if(cmd==FS_UNINIT){uninit++;return 0;}
  assert(0);return -1;
}
int main(void){
  usbfs f;
  for(unsigned n=16;n<=64;n*=2){
    assert(fs_setup(&f,1,n,3)==0&&f.opened==3);
    assert(fs_submit(&f,1,1)==0&&f.submitted==n);
    assert(fs_submit(&f,1,1)<0);
    empty=1;assert(fs_complete(&f,1)==0);empty=0;
    assert(fs_complete(&f,1)==2&&f.completed==n);
    assert(fs_submit(&f,1,2)==0);short_frame=1;assert(fs_complete(&f,1)<0);short_frame=0;
    assert(fs_submit(&f,1,0)==0);bad_status=1;assert(fs_complete(&f,1)<0);bad_status=0;
    assert(fs_cleanup(&f,1)==0);
  }
  fail_open=1;assert(fs_setup(&f,1,32,3)<0&&f.initialized);assert(fs_cleanup(&f,1)==0&&uninit==4);
  expected_slots=2;fail_open=0;assert(fs_setup(&f,1,24,2)==0&&f.opened==2);assert(fs_submit(&f,1,1)==0);assert(fs_complete(&f,1)==2);assert(fs_cleanup(&f,1)==0);
  assert(fs_setup(&f,1,8,2)<0&&!f.initialized);
  for(unsigned i=0;i<BENCH_MODES;i++){assert(bench_modes[i].request==1024);}
  assert(rate_bad(0,7972)==1&&rate_bad(1,7972)==2&&rate_bad(2,8000)==0);
  assert(BENCH_COUNT==3&&BENCH_SECONDS==30&&BENCH_WARMUP==8);
  assert(bench_id(0)==9&&bench_id(1)==10&&bench_id(2)==13);
  for(unsigned i=0;i<BENCH_COUNT;i++){assert(bench_id(i)==bench_order[i]);assert(bench_id(BENCH_COUNT+i)==bench_order[BENCH_COUNT-1-i]);assert(profile_valid(bench_order[i]));}
  expected_slots=3;assert(fs_setup(&f,1,16,3)==0);assert(fs_submit(&f,1,0)==0);drain_mode=1;drain_pending=1;uint64_t before=f.calls;assert(fs_drain(&f,1)==1&&f.completed==16&&f.calls-before==2);assert(fs_drain(&f,1)==0);drain_mode=0;assert(fs_cleanup(&f,1)==0);
  bench_sample a={.cpu={.at=1}},b={.cpu={.at=26},.packets=200000};
  assert(bench_pass(a,b,0)&&bench_pass(a,b,64));
  b.underruns=1;assert(!bench_pass(a,b,32));b.underruns=0;b.packets=199700;assert(!bench_pass(a,b,32));b.packets=190000;assert(!bench_pass(a,b,32));
  puts("USB_FS ABI, completion, partial-open cleanup and benchmark counter tests passed");
}
