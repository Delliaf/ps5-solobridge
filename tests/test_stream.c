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
static eng_ring r2;
static void exact_pcm(void){
  uint32_t seed=1234567;
  for(unsigned i=0;i<200000;i++){
    eng_frame x;uint32_t bits[2];
    for(unsigned j=0;j<2;j++){seed=1664525u*seed+1013904223u;bits[j]=seed;}
    memcpy(&x,bits,sizeof(x));
    uint8_t a[6],b[6];eng_pack(a,x);eng_pack_vector(b,x);assert(!memcmp(a,b,6));
  }
  eng_init(&r);eng_init(&r2);eng_player a,b;eng_player_init(&a);eng_player_init(&b);
  eng_profile(&a,1024,96,1);eng_profile(&b,1024,96,1);b.vector=1;
  float x[2048];
  for(unsigned k=0;k<200;k++){
    for(unsigned i=0;i<2048;i++)x[i]=(float)((int)(i%127)-63)/64;
    assert(eng_push(&r,x,1024)==1024&&eng_push(&r2,x,1024)==1024);
    for(unsigned n=0;n<10;n++){
      uint8_t p[16*ENG_BYTES],q[16*ENG_BYTES];
      for(unsigned j=0;j<16;j++)eng_packet(&r,&a,p+j*ENG_BYTES);
      eng_packets(&r2,&b,q,16);
      assert(!memcmp(p,q,sizeof(p)));assert(eng_fill(&r)==eng_fill(&r2));
    }
  }
  assert(a.preroll==b.preroll&&a.underruns==b.underruns&&a.skipped==b.skipped);
  puts("PCM scalar/SSE2 bit equality and batched ring tests passed");
}
int main(void) {
  exact_pcm();
  concurrent(); simulate(500); simulate(-500); simulate(0);
  puts("stream concurrency and drift tests passed");
}
