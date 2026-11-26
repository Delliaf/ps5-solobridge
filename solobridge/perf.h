#ifndef TTGK_PERF_H
#define TTGK_PERF_H
#include <stdint.h>
typedef struct {
  double at,user,sys;
  uint64_t writes,loops,sleeps;
  int valid;
} perf_sample;
typedef struct { double user_pct,sys_pct,write_rate,loop_rate,sleep_rate,seconds; int valid; } perf_rate;
static inline perf_rate perf_diff(perf_sample a,perf_sample b) {
  perf_rate r={0};
  double dt=b.at-a.at;
  if(dt<=0||b.writes<a.writes||b.loops<a.loops||b.sleeps<a.sleeps)return r;
  r.seconds=dt;r.write_rate=(b.writes-a.writes)/dt;r.loop_rate=(b.loops-a.loops)/dt;r.sleep_rate=(b.sleeps-a.sleeps)/dt;
  if(a.valid&&b.valid&&b.user>=a.user&&b.sys>=a.sys) {
    r.valid=1;r.user_pct=100*(b.user-a.user)/dt;r.sys_pct=100*(b.sys-a.sys)/dt;
  }
  return r;
}
#endif
