#ifndef SOLO_H
#define SOLO_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include "volume_logic.h"
#include "usbfs.h"
#define SOLO_RATE 48000
typedef struct {
  int ctl_fd,ep_fd,bus,addr,channels,subslot,bits,configured;
  uint16_t maxpkt;
  uint32_t rate;
  hw_volume_state hw;
  usbfs fs;
  unsigned batch;
  int worker_started,transaction,sem_initialized;
  sem_t volume_event;
  atomic_int event_ok;
  pthread_t volume_thread;
  atomic_int volume_wanted,volume_done,volume_stop,volume_error;
  atomic_uint_fast64_t volume_busy_ms;
} solo_t;
int solo_open(solo_t *s,int verbose,int volume,unsigned batch);
void solo_request_volume(solo_t *s,int volume,int ready);
int solo_volume_done(solo_t *s);
int solo_volume_raw(solo_t *s);
int solo_volume_error(solo_t *s);
int solo_write(solo_t *s,const void *pkt,size_t len);
void solo_close(solo_t *s);
#endif
