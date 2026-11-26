/* GPL-3.0; portable TTGK protocol and input logic. */
#ifndef BRIDGE_LOGIC_H
#define BRIDGE_LOGIC_H
#include <stdint.h>
#include <stddef.h>
#define TTGK_MUTE 256
#define TTGK_DEFAULT 68
#define PAD_MOD (0x400u | 0x4u)
#define PAD_UP 0x10u
#define PAD_RIGHT 0x20u
#define PAD_DOWN 0x40u
#define PAD_LEFT 0x80u
#define PAD_DIR 0xf0u
static inline int volume_valid(int v) {
  return v >= 0 && (v & ~0x1ff) == 0 && (v & 255) <= 148;
}
static inline int volume_units(int v) { return ((v & 255) - 148) * 128; }
static inline int volume_step(int v, int delta) {
  int n = (v & 255) + delta;
  if (n < 0) n = 0;
  if (n > 148) n = 148;
  return n | (v & TTGK_MUTE);
}
static inline uint32_t pcm_word(float v) {
  if (!(v == v)) return 0;
  if (v >= 1.0f) return 0x7fffffffu;
  if (v <= -1.0f) return 0x80000000u;
  return (uint32_t)(int32_t)((double)v * 2147483648.0);
}
typedef struct { uint64_t since,next; uint32_t previous; int phase; } hotkey_t;
static inline int hotkey(hotkey_t *h, uint32_t b, uint64_t ms, int v) {
  uint32_t dir=b&PAD_DIR;
  if ((b&PAD_MOD)!=PAD_MOD) { *h=(hotkey_t){0}; return v; }
  if (!h->phase) { h->phase=b==PAD_MOD?1:3; h->since=ms; return v; }
  if (h->phase==3) return v;
  if (h->phase==1) {
    if (b!=PAD_MOD) h->phase=3;
    else if (ms-h->since>=500) h->phase=2;
    return v;
  }
  if (b&~(PAD_MOD|PAD_DIR)) { h->phase=3; return v; }
  if (!dir || (dir&(dir-1))) { h->previous=dir; return v; }
  if (dir==h->previous && (dir==PAD_LEFT || dir==PAD_RIGHT || ms<h->next)) return v;
  h->previous=dir; h->next=ms+150;
  if (dir==PAD_UP) return volume_step(v,4);
  if (dir==PAD_DOWN) return volume_step(v,-4);
  if (dir==PAD_LEFT) return v|TTGK_MUTE;
  if (dir==PAD_RIGHT) return v&~TTGK_MUTE;
  return v;
}
/* Known high-speed TTGK topology; reject unsupported layouts. */
static inline int ttgk_config(const uint8_t *b, size_t n) {
  if (n<9 || b[0]!=9 || b[1]!=2) return 0;
  size_t end=b[2]|((size_t)b[3]<<8);
  if (end<9 || end>n || b[5]!=1) return 0;
  int ac=0,version=0,clock=0,feature=0,input=0,output=0,ep=0;
  int iface=-1,alt=-1,channels=0,format=0,terminal=0;
  for (size_t i=0;i<end;) {
    if (end-i<2 || b[i]<2 || b[i]>end-i) return 0;
    const uint8_t *d=b+i; size_t l=d[0];
    if (d[1]==4) {
      if (l<9) return 0;
      iface=(d[5]==1 && d[7]==0x20)?d[2]:-1;
      alt=d[3]; channels=format=terminal=0;
      if (iface==0 && alt==0 && d[6]==1) ac=1;
      if ((iface==0 && d[6]!=1)||(iface==1 && d[6]!=2)) iface=-1;
    } else if (d[1]==0x24 && l>=3 && iface==0 && alt==0) {
      if (d[2]==1 && l>=9 && d[3]==0 && d[4]==2) version=1;
      if (d[2]==10 && l>=8 && d[3]==9 && (d[5]&3)==3) clock=1;
      if (d[2]==2 && l>=17 && d[3]==1 && d[7]==9 && d[8]==2) input=1;
      if (d[2]==3 && l>=12 && d[3]==3 && d[7]==2 && d[8]==9) output=1;
      if (d[2]==6 && l>=18 && d[3]==2 && d[4]==1 &&
          (d[5]&3)==3 && (d[9]&12)==12 && (d[13]&12)==12) feature=1;
    } else if (d[1]==0x24 && l>=3 && iface==1 && alt==2) {
      if (d[2]==1 && l>=16) {
        terminal=d[3]; channels=d[10];
        if (d[5]!=1 || d[6]!=1 || d[7] || d[8] || d[9]) return 0;
      }
      if (d[2]==2 && l>=6 && d[3]==1 && d[4]==3 && d[5]==24) format=1;
    } else if (d[1]==5 && iface==1 && alt==2) {
      if (l<7 || d[2]!=1 || d[3]!=0x0d || d[4]!=0x20 || d[5]!=1 ||
          d[6]!=1 || channels!=2 || terminal!=1 || !format) return 0;
      ep=1;
    }
    i+=l;
  }
  return ac&&version&&clock&&feature&&input&&output&&ep;
}
#endif
