#ifndef SOURCE_H
#define SOURCE_H
#include "engine.h"
#include <pthread.h>
typedef struct {
  pthread_t thread;
  atomic_int stop,enable,active,fatal;
  atomic_uint epoch,request_generation;
  atomic_ulong overruns,dropped;
  eng_ring ring;
} source_t;
int source_start(source_t *s);
void source_enable(source_t *s,int enabled);
void source_stop(source_t *s);
#endif
