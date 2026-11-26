/* End-to-end test of the actual local probe worker, not a PS5 ABI test. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../solobridge/probe.h"
#include "../solobridge/probe_logic.h"
#ifndef PROBE_ROOT
#error use a disposable host-test directory
#endif
#define OUT PROBE_ROOT "/tempest"
#define REQUEST PROBE_ROOT "/tempest_probe.request"
static float frames[1024*2];
void log_line(const char *fmt,...) { (void)fmt; }
static void create(const char *path,const char *s) {
  FILE *fp=fopen(path,"wb"); assert(fp);
  assert(fputs(s,fp)>=0); assert(!fclose(fp));
}
static int wait_status(const char *s) {
  for (int i=0;i<100;i++) {
    FILE *fp=fopen(OUT "/status.txt","r");
    if (fp) {
      char line[128]={0}; int ok=fgets(line,sizeof(line),fp)!=NULL;
      fclose(fp); if (ok && strstr(line,s)) return 1;
    }
    usleep(20000);
  }
  return 0;
}
static void submit(const char *s) {
  create(REQUEST ".tmp",s); assert(!rename(REQUEST ".tmp",REQUEST));
}
int main(void) {
  assert(probe_start()==0); probe_feed(frames,1024); probe_stop();
  create(PROBE_ROOT "/enable_tempest_probe","");
  create(REQUEST,"off_a"); /* stale request must not execute */
  assert(probe_start()==1); assert(wait_status("idle"));
  assert(access(REQUEST,F_OK)!=0);
  submit("../x"); assert(wait_status("invalid request"));
  submit("off_a\n"); assert(wait_status("armed"));
  for (unsigned i=0;i<1024;i++) {frames[i*2]=0.5f;frames[i*2+1]=-0.5f;}
  for (unsigned i=0;i<375;i++) probe_feed(frames,1024);
  assert(wait_status("complete: off_a.wav"));
  FILE *fp=fopen(OUT "/off_a.wav","rb"); assert(fp);
  uint8_t h[44],expected[44],p[4]; probe_wav_header(expected,48000*8);
  assert(fread(h,1,44,fp)==44 && !memcmp(h,expected,44));
  assert(fread(p,1,4,fp)==4 && p[0]==0 && p[1]==0x40 && p[2]==0 && p[3]==0xc0);
  assert(!fseek(fp,0,SEEK_END) && ftell(fp)==44+48000*8*4);
  fclose(fp);
  submit("on_a"); assert(wait_status("armed"));
  probe_feed(frames,1024); probe_discontinuity();
  assert(wait_status("failed: capture interrupted"));
  assert(access(OUT "/on_a.wav",F_OK)!=0);
  submit("on_b"); assert(wait_status("armed")); probe_stop();
  assert(access(OUT "/on_b.wav",F_OK)!=0);
  puts("Tempest probe lifecycle tests passed");
}
