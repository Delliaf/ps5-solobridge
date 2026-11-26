#ifndef TTGK_BENCH_H
#define TTGK_BENCH_H
#include <stdint.h>
#include <stdio.h>
#include "perf.h"
typedef struct {unsigned batch,slots,request,reserve,poll,adaptive,trim,efficient,vector;} bench_profile;
/* Indexes are version-local and used by the web UI. No automatic persistence. */
static const bench_profile bench_modes[]={
  {0,3,1024,192,2000,0,0,0,0}, /* known safe fallback */
  {16,2,1024,96,2000,1,1,1,0}, /* previous profile 10 control */
  {16,3,1024,96,2000,1,1,1,1},
  {24,2,1024,96,2000,1,1,1,1},
  {24,3,1024,96,2000,1,1,1,1},
  {32,2,1024,96,2000,1,1,1,1},
  {32,3,1024,96,2000,1,1,1,1},
  {16,3,1024,48,1000,1,1,1,1},
  {16,2,1024,96,2000,1,1,1,1}
};
#define BENCH_MODES (sizeof(bench_modes)/sizeof(bench_modes[0]))
#define BENCH_WARMUP 8
#define BENCH_SECONDS 45
static inline unsigned bench_id(unsigned step){
  unsigned i=step%BENCH_MODES;
  return (step/BENCH_MODES)%2?BENCH_MODES-1-i:i;
}
static inline int profile_valid(unsigned i){return i<BENCH_MODES;}
/* Stream rate guard uses two consecutive bad windows, not a single jittery
 * boundary. Errors and drops can independently trigger fallback. */
static inline unsigned rate_bad(unsigned old,double rate){
  return rate<7975||rate>8025?(old<2?old+1:2):0;
}
typedef struct {uint64_t reads,empty,frames,gaps,gap_us;unsigned min,max,gap_max;} capture_stats;
#define QUEUE_BINS 512
typedef struct {uint64_t bin[QUEUE_BINS],n;double sum,max;} queue_stats;
static inline void queue_add(queue_stats *s,double ms){
  if(ms<0)return;
  unsigned i=ms>=255.5?QUEUE_BINS-1:(unsigned)(ms*2);
  s->bin[i]++;s->n++;s->sum+=ms;if(ms>s->max)s->max=ms;
}
static inline double queue_p95(const queue_stats *s){
  if(!s->n)return 0;
  uint64_t need=(s->n*95+99)/100,seen=0;
  for(unsigned i=0;i<QUEUE_BINS;i++){seen+=s->bin[i];if(seen>=need)return (i+1)*0.5;}
  return 256;
}
typedef struct {perf_sample cpu;uint64_t packets,underruns,skipped,late,blocked,drop,overruns,reconnects,fs_calls;unsigned epoch;} bench_sample;
static inline int bench_pass(bench_sample a,bench_sample b,unsigned batch){
  double dt=b.cpu.at-a.cpu.at;
  if(dt<24||b.packets<a.packets)return 0;
  double rate=(b.packets-a.packets)/dt;
  double tol=batch?batch*3/dt+16:16;
  return rate>=8000-tol&&rate<=8000+tol&&a.underruns==b.underruns&&a.skipped==b.skipped&&a.late==b.late&&a.blocked==b.blocked&&a.drop==b.drop&&a.overruns==b.overruns&&a.reconnects==b.reconnects&&a.epoch==b.epoch;
}
#endif
