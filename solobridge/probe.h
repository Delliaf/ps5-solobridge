#ifndef PROBE_H
#define PROBE_H
#include <stddef.h>
/* No thread, buffer, recording or filesystem polling unless opted in at launch. */
int probe_start(void);
void probe_feed(const float *pcm,size_t frames);
void probe_discontinuity(void);
void probe_stop(void);
#endif
