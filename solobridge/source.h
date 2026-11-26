#ifndef SOURCE_H
#define SOURCE_H
#include "engine.h"
#include <pthread.h>
typedef struct {
  pthread_t thread;
  atomic_int stop,fatal;
  atomic_uint epoch,request_generation,active_generation;
  atomic_ulong overruns,dropped;
  atomic_uint request_frames,poll_us,adaptive;
  atomic_uint read_min,read_max,gap_max_us;
  atomic_ulong reads,empty,frames,gap_sum_us,gaps;
  eng_ring ring;
} source_t;
int source_start(source_t *s);
void source_enable(source_t *s,int enabled);
static inline int source_active(source_t *s) {
  unsigned req=atomic_load(&s->request_generation);
  return (req&1u) && req==atomic_load(&s->active_generation);
}
void source_stop(source_t *s);
#endif
