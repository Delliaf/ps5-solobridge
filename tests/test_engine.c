#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../solobridge/engine.h"
static eng_ring r;
static float samples[ENG_RING*2];
int main(void) {
  eng_init(&r); eng_player p; eng_player_init(&p); uint8_t b[ENG_BYTES];
  eng_packet(&r,&p,b); assert(!p.playing && p.silent==1);
  for (unsigned i=0;i<ENG_BYTES;i++) assert(b[i]==0);
  for (unsigned i=0;i<ENG_RING;i++) { samples[2*i]=0.5f; samples[2*i+1]=-0.5f; }
  assert(eng_push(&r,samples,ENG_RING)==ENG_RING);
  assert(eng_push(&r,samples,1)==0);
  eng_packet(&r,&p,b); assert(p.playing && p.skipped>0);
  for (int i=0;i<10;i++) eng_packet(&r,&p,b);
  assert(b[0]==0 && b[1]==0 && b[2]==0x40);
  assert(b[3]==0 && b[4]==0 && b[5]==0xc0);
  eng_discard(&r,eng_fill(&r)-7); eng_packet(&r,&p,b);
  assert(!p.playing && p.underruns==1 && p.target==288);
  for (int i=0;i<20;i++) eng_packet(&r,&p,b);
  for (unsigned i=0;i<ENG_BYTES;i++) assert(b[i]==0);
  eng_reset(&r,&p); assert(!eng_fill(&r) && !p.playing);
  assert(eng_push(&r,samples,2048)==2048);
  eng_packet(&r,&p,b); p.fade=0;
  p.delta=164; p.phase=65530; size_t fill=eng_fill(&r);
  eng_packet(&r,&p,b); assert(eng_fill(&r)==fill-7);
  p.delta=-164; p.phase=0; fill=eng_fill(&r);
  eng_packet(&r,&p,b); assert(eng_fill(&r)==fill-5);
  eng_pacer q; eng_pace_init(&q,1.0);
  for (int i=0;i<48;i++) { assert(eng_pace_need(&q)); eng_pace_write(&q); }
  assert(!eng_pace_need(&q)); eng_pace_block(&q); assert(q.blocked==1 && q.queued==48);
  eng_pace_tick(&q,1.001); assert(q.queued>39.9 && q.queued<40.1);
  eng_pace_tick(&q,10); assert(q.queued==0 && q.late==1);
  puts("engine tests passed");
}
