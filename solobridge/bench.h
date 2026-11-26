#ifndef TTGK_BENCH_H
#define TTGK_BENCH_H
#include <stdint.h>
#include <stdio.h>
#include "perf.h"
typedef struct {unsigned batch,slots,request,reserve,poll,adaptive,trim,efficient,vector;} bench_profile;
static const bench_profile bench_modes[]={
  {0,3,1024,192,2000,0,0,0,0},
  {64,3,1024,192,2000,0,0,0,0},
  {32,2,1024,96,2000,0,1,1,0},
  {24,2,1024,96,2000,0,1,1,0},
  {16,2,1024,96,2000,0,1,1,0},
  {8,2,1024,96,2000,0,1,1,0},
  {16,2,512,96,1000,0,1,1,0},
  {16,2,256,96,1000,0,1,1,0},
  {16,2,128,96,500,0,1,1,0},
  {16,2,1024,96,500,0,1,1,0},
  {16,2,1024,96,2000,1,1,1,0},
  {16,2,1024,96,2000,0,1,1,1}
};
#define BENCH_MODES (sizeof(bench_modes)/sizeof(bench_modes[0]))
typedef struct {uint64_t reads,empty,frames,gaps,gap_us;unsigned min,max,gap_max;} capture_stats;
typedef struct {perf_sample cpu;uint64_t packets,underruns,skipped,late,blocked,drop,overruns,reconnects,fs_calls;unsigned epoch;} bench_sample;
static inline int bench_pass(bench_sample a,bench_sample b,unsigned batch){
  double dt=b.cpu.at-a.cpu.at;
  if(dt<24||b.packets<a.packets)return 0;
  double rate=(b.packets-a.packets)/dt;
  double tol=batch?batch*3/dt+16:16;
  return rate>=8000-tol&&rate<=8000+tol&&a.underruns==b.underruns&&a.skipped==b.skipped&&a.late==b.late&&a.blocked==b.blocked&&a.drop==b.drop&&a.overruns==b.overruns&&a.reconnects==b.reconnects&&a.epoch==b.epoch;
}
#endif
