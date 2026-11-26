/* GPL-3.0. Single producer / single consumer PCM ring; no locks in audio path. */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "bridge_logic.h"
#define ENG_RATE 48000
#define ENG_RING 16384u
#define ENG_RECORD 1024u
#define ENG_FRAMES 6u
#define ENG_BYTES 36u
#define ENG_AHEAD 48.0
#define ENG_Q 65536
typedef struct { float l,r; } eng_frame;
typedef struct {
  atomic_uint_fast64_t rd,wr;
  eng_frame data[ENG_RING];
} eng_ring;
static inline void eng_init(eng_ring *r) {
  atomic_init(&r->rd,0); atomic_init(&r->wr,0);
}
static inline size_t eng_fill(eng_ring *r) {
  uint64_t rd=atomic_load_explicit(&r->rd,memory_order_relaxed);
  uint64_t wr=atomic_load_explicit(&r->wr,memory_order_acquire);
  return (size_t)(wr-rd);
}
/* Producer only: never overwrite data the consumer may still read. */
static inline size_t eng_push(eng_ring *r,const float *src,size_t n) {
  uint64_t w=atomic_load_explicit(&r->wr,memory_order_relaxed);
  uint64_t d=atomic_load_explicit(&r->rd,memory_order_acquire);
  size_t free_n=ENG_RING-(size_t)(w-d);
  if (n>free_n) n=free_n;
  for (size_t i=0;i<n;i++) r->data[(w+i)&(ENG_RING-1)]=(eng_frame){src[i*2],src[i*2+1]};
  atomic_store_explicit(&r->wr,w+n,memory_order_release);
  return n;
}
/* Consumer only. */
static inline void eng_discard(eng_ring *r,size_t n) {
  uint64_t d=atomic_load_explicit(&r->rd,memory_order_relaxed);
  size_t fill=eng_fill(r); if (n>fill) n=fill;
  atomic_store_explicit(&r->rd,d+n,memory_order_release);
}
typedef struct {
  int playing,phase,delta,fade;
  unsigned target,window,min_fill,stable;
  uint64_t underruns,skipped,packets,silent;
  eng_frame last,from;
} eng_player;
static inline void eng_player_init(eng_player *p) {
  memset(p,0,sizeof(*p)); p->target=192; p->min_fill=ENG_RING;
}
static inline void eng_reset(eng_ring *r,eng_player *p) {
  eng_discard(r,eng_fill(r));
  p->playing=p->phase=p->delta=0;
  p->fade=0; p->last=(eng_frame){0,0};
  p->window=0; p->min_fill=ENG_RING; p->stable=0;
}
static inline float eng_clean(float v) {
  if (!(v==v)) return 0;
  if (v>1) return 1;
  if (v< -1) return -1;
  return v;
}
static inline void eng_pack(uint8_t *out,eng_frame f) {
  uint32_t l=pcm_word(f.l)>>8,r=pcm_word(f.r)>>8;
  out[0]=l;out[1]=l>>8;out[2]=l>>16;
  out[3]=r;out[4]=r>>8;out[5]=r>>16;
}
static inline void eng_transition(eng_player *p) {
  p->from=p->last; p->fade=48; /* 1 ms, only discontinuities, not volume control */
}
static inline eng_frame eng_fade(eng_player *p,eng_frame f) {
  f.l=eng_clean(f.l); f.r=eng_clean(f.r);
  if (p->fade>0) {
    float a=(49-p->fade)/48.0f;
    f.l=p->from.l+(f.l-p->from.l)*a;
    f.r=p->from.r+(f.r-p->from.r)*a;
    p->fade--;
  }
  p->last=f; return f;
}
/* Fixed six frames/microframe. Fractional phase compensates clock mismatch
 * with interpolation instead of abrupt frame deletion/duplication. */
static inline void eng_packet(eng_ring *r,eng_player *p,uint8_t out[ENG_BYTES]) {
  size_t fill=eng_fill(r);
  if (!p->playing && fill>=ENG_RECORD+p->target+2) {
    p->playing=1; p->phase=p->delta=0; p->window=0; p->min_fill=ENG_RING;
    eng_transition(p);
  }
  if (p->playing && fill>4096) {
    size_t skip=fill-(ENG_RECORD+p->target);
    eng_discard(r,skip); p->skipped+=skip; fill-=skip;
    p->phase=p->delta=0; p->window=0; p->min_fill=ENG_RING; eng_transition(p);
  }
  /* A complete packet can consume at most seven source frames plus lookahead. */
  if (p->playing && fill<ENG_FRAMES+2) {
    p->playing=0; p->underruns++; p->stable=0;
    if (p->target<768) p->target+=96;
    p->phase=p->delta=0; eng_transition(p);
  }
  p->packets++;
  if (!p->playing) {
    p->silent++;
    for (unsigned i=0;i<ENG_FRAMES;i++) eng_pack(out+i*6,eng_fade(p,(eng_frame){0,0}));
    return;
  }
  if (fill<p->min_fill) p->min_fill=(unsigned)fill;
  if (++p->window>=4000) {
    /* min queue depth over a 0.5s window, not the sawtooth average. */
    int error=(int)p->min_fill-(int)p->target;
    int want=error*2;
    if (want>164) want=164;
    if (want< -164) want=-164;
    p->delta=(3*p->delta+want)/4;
    if (++p->stable>=120) {
      if (p->target>192) p->target-=96;
      p->stable=0;
    }
    p->window=0; p->min_fill=ENG_RING;
  }
  uint64_t rd=atomic_load_explicit(&r->rd,memory_order_relaxed);
  for (unsigned i=0;i<ENG_FRAMES;i++) {
    eng_frame a=r->data[rd&(ENG_RING-1)], b=r->data[(rd+1)&(ENG_RING-1)];
    a.l=eng_clean(a.l);a.r=eng_clean(a.r);b.l=eng_clean(b.l);b.r=eng_clean(b.r);
    float t=(float)p->phase/ENG_Q;
    eng_pack(out+i*6,eng_fade(p,(eng_frame){a.l+(b.l-a.l)*t,a.r+(b.r-a.r)*t}));
    unsigned step=(unsigned)(p->phase+ENG_Q+p->delta);
    rd+=step/ENG_Q; p->phase=(int)(step%ENG_Q);
  }
  atomic_store_explicit(&r->rd,rd,memory_order_release);
}
/* Time-based queue estimate only; not a measurement of the USB kernel FIFO.
 * Bounded credits prevent large bursts after stalls or rest-mode gaps. */
typedef struct { double last,queued; uint64_t late,blocked; } eng_pacer;
static inline void eng_pace_init(eng_pacer *p,double now) { memset(p,0,sizeof(*p)); p->last=now; }
static inline void eng_pace_tick(eng_pacer *p,double now) {
  double dt=now-p->last; p->last=now;
  if (dt<0) dt=0;
  p->queued-=dt*8000.0;
  if (p->queued<0) { if (p->queued< -1) p->late++; p->queued=0; }
}
static inline int eng_pace_need(eng_pacer *p) { return p->queued+1<=ENG_AHEAD; }
static inline void eng_pace_write(eng_pacer *p) { p->queued+=1; }
/* Wait until about eight packet credits are available, preserving the
 * 48-packet upper bound and leaving roughly 5ms queued at nominal timing.
 * Queue depth is estimated, so this is selectable rather than a guarantee. */
static inline unsigned eng_pace_sleep(const eng_pacer *p,double now,int economy) {
  if(!economy)return 250;
  double elapsed=now-p->last;
  if(elapsed<0)elapsed=0;
  double q=p->queued-elapsed*8000.0;
  double us=(q-(ENG_AHEAD-8.0))*125.0;
  if(us<=0)return 0;
  /* Round up, avoiding a zero-delay spin due to truncation. */
  return us>=999.0?1000:(unsigned)(us+0.999999);
}
static inline void eng_pace_block(eng_pacer *p) { p->blocked++; }
#endif
