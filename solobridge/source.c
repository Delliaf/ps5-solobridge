/* GPL-3.0. AVCAP lifecycle and blocking calls stay off the USB writer. */
#include "source.h"
#include "capture.h"
#include "log.h"
#include "probe.h"
#include <time.h>
#include <unistd.h>
static double seconds(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec/1e9;
}
static void inactive(source_t *s) {
  probe_discontinuity();
  atomic_store(&s->active_generation,0);
  atomic_fetch_add(&s->epoch,1);
  capture_close();
}
static void *run(void *arg) {
  source_t *s=arg; int loaded=0,active=0;
  unsigned generation=atomic_load(&s->request_generation);
  float rec[CAPTURE_RECORD/sizeof(float)];
  double retry=0,last=0,last_record=0;
  unsigned request=ENG_RECORD,poll_us=2000,adaptive=0,last_frames=ENG_RECORD;
  while (!atomic_load(&s->stop)) {
    unsigned next=atomic_load(&s->request_generation);
    if (next!=generation) {
      if (active) { inactive(s); active=0; }
      generation=next; retry=0;last_record=0;
      request=atomic_load(&s->request_frames);poll_us=atomic_load(&s->poll_us);adaptive=atomic_load(&s->adaptive);
    }
    if (!(generation&1u)) {
      if (active) { inactive(s); active=0; }
      retry=0; usleep(50000); continue;
    }
    double now=seconds();
    if (!active) {
      if (now<retry) { usleep(20000); continue; }
      int r=loaded?capture_restart():capture_open();
      if (r<0) { atomic_store(&s->fatal,1); log_line("capture: SDK/module init failed"); break; }
      if (!loaded) loaded=1;
      if (!r) { retry=seconds()+0.5; continue; }
      if (atomic_load(&s->request_generation)!=generation) { inactive(s); continue; }
      atomic_fetch_add(&s->epoch,1); atomic_store(&s->active_generation,generation); active=1; last=seconds();
      last_record=0;atomic_store(&s->read_min,0);atomic_store(&s->read_max,0);atomic_store(&s->gap_max_us,0);
      log_line("capture: started request_frames=%u poll_us=%u adaptive=%u",request,poll_us,adaptive);
    }
    int n=0,overrun=0; long old=capture_overruns();
    for (int i=0;i<8 && atomic_load(&s->request_generation)==generation && !atomic_load(&s->stop);i++) {
      n=capture_read(rec,request*2*sizeof(float));
      atomic_fetch_add(&s->reads,1);
      if (n<=0){if(!n)atomic_fetch_add(&s->empty,1);break;}
      if (atomic_load(&s->request_generation)!=generation) break;
      size_t frames=(size_t)n/(sizeof(float)*CAPTURE_CHANNELS);
      double arrived=seconds();
      unsigned lo=atomic_load(&s->read_min),hi=atomic_load(&s->read_max);
      if(!lo||frames<lo)atomic_store(&s->read_min,(unsigned)frames);
      if(frames>hi)atomic_store(&s->read_max,(unsigned)frames);
      if(last_record){double d=(arrived-last_record)*1e6;unsigned gap=d>10000000?10000000:(unsigned)d;
        atomic_fetch_add(&s->gaps,1);atomic_fetch_add(&s->gap_sum_us,gap);
        if(gap>atomic_load(&s->gap_max_us))atomic_store(&s->gap_max_us,gap);}
      last_record=arrived;last_frames=(unsigned)frames;atomic_fetch_add(&s->frames,frames);
      probe_feed(rec,frames);
      size_t put=eng_push(&s->ring,rec,frames);
      if (put<frames) atomic_fetch_add(&s->dropped,frames-put);
      last=seconds();
    }
    if (capture_overruns()!=old) {
      probe_discontinuity();
      overrun=1; atomic_store(&s->overruns,(unsigned long)capture_overruns());
      atomic_fetch_add(&s->epoch,1);
    }
    if (n<0 || seconds()-last>2.0) {
      log_line("capture: %s, restarting",n<0?"session ended":"no records for 2s");
      inactive(s); active=0; retry=seconds()+0.5;
    } else if(n<=0||overrun){
      unsigned delay=poll_us;
      if(adaptive&&last_record&&!overrun){
        double remaining=(last_record+last_frames/48000.0-seconds())*1e6-2000;
        if(remaining>delay)delay=remaining>10000?10000:(unsigned)remaining;
      }
      usleep(delay);
    }
  }
  if (active) inactive(s);
  return NULL;
}
int source_start(source_t *s) {
  atomic_init(&s->stop,0); atomic_init(&s->active_generation,0);
  atomic_init(&s->fatal,0); atomic_init(&s->epoch,0); atomic_init(&s->request_generation,0); atomic_init(&s->overruns,0);
  atomic_init(&s->dropped,0); eng_init(&s->ring);
  atomic_init(&s->request_frames,1024);atomic_init(&s->poll_us,2000);atomic_init(&s->adaptive,0);
  atomic_init(&s->read_min,0);atomic_init(&s->read_max,0);atomic_init(&s->gap_max_us,0);
  atomic_init(&s->reads,0);atomic_init(&s->empty,0);atomic_init(&s->frames,0);atomic_init(&s->gaps,0);atomic_init(&s->gap_sum_us,0);
  return pthread_create(&s->thread,NULL,run,s)?-1:0;
}
/* Main is the sole requester: low bit is enabled; each transition
 * changes the generation in one atomic operation. A late capture start
 * cannot be mistaken for a newly opened USB session. */
void source_enable(source_t *s,int enabled) {
  unsigned old=atomic_load(&s->request_generation);
  if ((old&1u)!=(unsigned)!!enabled) atomic_store(&s->request_generation,old+1);
}
void source_stop(source_t *s) { atomic_store(&s->stop,1); pthread_join(s->thread,NULL); }
