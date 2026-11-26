#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../solobridge/engine.h"
static eng_ring r;
#define N 200000u
static void *producer(void *arg) {
  (void)arg;
  for (unsigned i=0;i<N;) {
    float x[2]={(float)i,-(float)i};
    if (eng_push(&r,x,1)) i++; else sched_yield();
  }
  return NULL;
}
static void concurrent(void) {
  eng_init(&r); pthread_t t; assert(!pthread_create(&t,NULL,producer,NULL));
  for (unsigned i=0;i<N;) {
    size_t n=eng_fill(&r);
    if (!n) { sched_yield(); continue; }
    assert(n<=ENG_RING);
    uint64_t d=atomic_load(&r.rd);
    eng_frame x=r.data[d&(ENG_RING-1)];
    assert(x.l==(float)i && x.r==-(float)i);
    eng_discard(&r,1); i++;
  }
  assert(!pthread_join(t,NULL) && !eng_fill(&r));
}
/* 60 seconds of source/USB clock mismatch; packet counts fixed at 8000/s.
 * This deliberately models capture records, not individual PCM arrivals. */
static void simulate(double ppm) {
  eng_init(&r); eng_player p; eng_player_init(&p);
  float x[ENG_RECORD*2]={0};
  uint8_t b[ENG_BYTES];
  double due=0,interval=ENG_RECORD*8000.0/(ENG_RATE*(1.0+ppm/1000000.0));
  for (unsigned i=0;i<60*8000u;i++) {
    if (i>=due) { assert(eng_push(&r,x,ENG_RECORD)==ENG_RECORD); due+=interval; }
    eng_packet(&r,&p,b);
    assert(eng_fill(&r)<=ENG_RING);
    assert(p.phase>=0 && p.phase<ENG_Q);
  }
  assert(p.playing && p.underruns<=2 && p.skipped==0);
  printf("drift %+.0f ppm: underruns=%llu fill=%zu correction=%d\n",
    ppm,(unsigned long long)p.underruns,eng_fill(&r),p.delta);
}
int main(void) {
  concurrent(); simulate(500); simulate(-500); simulate(0);
  puts("stream concurrency and drift tests passed");
}
