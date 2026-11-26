#ifndef TTGK_WEB_H
#define TTGK_WEB_H
#include <stdint.h>
#include "perf.h"
typedef struct {
  int usb,capture,playing,gate,hw,ring,phase;
  double queue;
  uint64_t underruns,late,blocked,reconnects,source_drop,skipped,writes;
  perf_rate perf;
  unsigned batch,slots,read_min,read_max,gap_max_us,profile,selected,reserve;
  double fs_rate,packet_rate,empty_rate;
} web_audio;
int web_bench_command(void);
void web_bench_begin(void);
void web_bench_line(const char *fmt,...);
void web_bench_end(void);
void web_set_economy(int on);
int web_get_economy(void);
int web_start(void);
int web_active(void);
int web_snapshot_requested(void);
void web_publish(const web_audio *s);
void web_stop(void);
#endif
