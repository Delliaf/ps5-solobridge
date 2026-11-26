/* GPL-3.0. Native Tempest diagnostic, not a spatial renderer. */
#ifndef PROBE_LOGIC_H
#define PROBE_LOGIC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "bridge_logic.h"
#define PROBE_RATE 48000u
#define PROBE_MAX_SECONDS 20u
/* Fixed filenames only. A request never controls filesystem paths. */
static inline unsigned probe_duration(const char *s) {
  static const char *const labels[]={
    "off_a","off_b","on_a","on_b","measurement_off_a","measurement_off_b",
    "measurement_on_a","measurement_on_b"
  };
  if (!strcmp(s,"calibration")) return 20;
  for (size_t i=0;i<sizeof(labels)/sizeof(labels[0]);i++)
    if (!strcmp(s,labels[i])) return 8;
  return 0;
}
static inline void probe_le16(uint8_t *p,uint16_t v) {p[0]=v;p[1]=v>>8;}
static inline void probe_le32(uint8_t *p,uint32_t v) {p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static inline void probe_wav_header(uint8_t h[44],uint32_t frames) {
  memset(h,0,44);
  memcpy(h,"RIFF",4); probe_le32(h+4,36+frames*4);
  memcpy(h+8,"WAVEfmt ",8); probe_le32(h+16,16);
  probe_le16(h+20,1); probe_le16(h+22,2);
  probe_le32(h+24,PROBE_RATE); probe_le32(h+28,PROBE_RATE*4);
  probe_le16(h+32,4); probe_le16(h+34,16);
  memcpy(h+36,"data",4); probe_le32(h+40,frames*4);
}
static inline void probe_pcm(uint8_t *out,const float *src,size_t frames) {
  for (size_t i=0;i<frames*2;i++) probe_le16(out+2*i,(uint16_t)(pcm_word(src[i])>>16));
}
#endif
