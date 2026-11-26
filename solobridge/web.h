#ifndef TTGK_WEB_H
#define TTGK_WEB_H
#include <stdint.h>
typedef struct {
  int usb,capture,playing,gate,hw,ring,phase;
  double queue;
  uint64_t underruns,late,blocked,reconnects,source_drop;
} web_audio;
int web_start(void);
int web_active(void);
int web_snapshot_requested(void);
void web_publish(const web_audio *s);
void web_stop(void);
#endif
