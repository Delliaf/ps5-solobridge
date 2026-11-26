#ifndef SOLO_H
#define SOLO_H
#include <stddef.h>
#include <stdint.h>
#define SOLO_RATE 48000
typedef struct {
  int ctl_fd,ep_fd,bus,addr,channels,subslot,bits,configured;
  uint16_t maxpkt;
  uint32_t rate;
} solo_t;
int solo_open(solo_t *s,int verbose,int volume);
int solo_set_volume(solo_t *s,int volume);
int solo_write(solo_t *s,const void *pkt,size_t len);
void solo_close(solo_t *s);
#endif
