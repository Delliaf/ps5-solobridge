#ifndef TTGK_WEB_H
#define TTGK_WEB_H
#include <stdint.h>
#include "perf.h"
typedef struct {
  int usb,capture,playing,gate,hw,ring,phase;
  double queue;
  uint64_t underruns,late,blocked,reconnects,source_drop,skipped,writes;
  perf_rate perf;
} web_audio;
void web_set_economy(int on);
int web_get_economy(void);
int web_start(void);
int web_active(void);
int web_snapshot_requested(void);
void web_publish(const web_audio *s);
void web_stop(void);
#endif
