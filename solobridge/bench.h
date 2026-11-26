#ifndef TTGK_BENCH_H
#define TTGK_BENCH_H
#include <stdint.h>
#include <stdio.h>
#include "perf.h"
#define BENCH_MODES 4
static const unsigned bench_modes[BENCH_MODES]={0,16,32,64};
typedef struct {perf_sample cpu;uint64_t packets,underruns,skipped,late,blocked,drop,overruns,reconnects,fs_calls;unsigned epoch;} bench_sample;
static inline int bench_pass(bench_sample a,bench_sample b,unsigned batch){
  double dt=b.cpu.at-a.cpu.at;
  if(dt<24||b.packets<a.packets)return 0;
  double rate=(b.packets-a.packets)/dt;
  double tol=batch?batch*3/dt+16:16;
  return rate>=8000-tol&&rate<=8000+tol&&a.underruns==b.underruns&&a.skipped==b.skipped&&a.late==b.late&&a.blocked==b.blocked&&a.drop==b.drop&&a.overruns==b.overruns&&a.reconnects==b.reconnects&&a.epoch==b.epoch;
}
#endif
