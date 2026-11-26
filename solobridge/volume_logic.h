/* GPL-3.0. Hardware-only, mockable volume transaction. */
#ifndef VOLUME_LOGIC_H
#define VOLUME_LOGIC_H
#include "bridge_logic.h"
typedef struct { int volume,mute; } hw_volume_state;
typedef struct {
  int (*mute)(void *,int);
  int (*channel)(void *,int,int);
  int (*allow_unmute)(void *);
} hw_volume_ops;
/* 0 verified, 1 superseded before unmute, -1 uncertain hardware state.
 * Callbacks verify readback. On any failure caller closes output immediately
 * and attempts a best-effort hardware mute. */
static inline int hw_volume_apply(hw_volume_state *s,const hw_volume_ops *ops,void *ctx,int v) {
  if (!volume_valid(v)) return -1;
  int mute=!!(v&TTGK_MUTE),level=v&255;
  if (mute && s->mute!=1) {
    if (ops->mute(ctx,1)<0) goto fail;
    s->mute=1;
  }
  if (s->volume!=level) {
    if (ops->channel(ctx,1,volume_units(level))<0 ||
        ops->channel(ctx,2,volume_units(level))<0) goto fail;
    s->volume=level;
  }
  if (!mute && s->mute!=0) {
    if (!ops->allow_unmute(ctx)) return 1;
    if (ops->mute(ctx,0)<0) goto fail;
    s->mute=0;
  }
  return 0;
fail:
  s->volume=s->mute=-1;
  return -1;
}
#endif
