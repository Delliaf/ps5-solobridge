#include <assert.h>
#include <stdio.h>
#include "../solobridge/bridge_logic.h"
#include "ttgk_descriptor.h"
#include <string.h>
#include <math.h>
int main(void) {
  assert(pcm_word(1.0f)==0x7fffffffu && pcm_word(-1.0f)==0x80000000u);
  assert(pcm_word(0.0f)==0 && pcm_word(0.5f)==0x40000000u);
  assert(volume_units(0)==-18944 && volume_units(148)==0);
  assert(volume_step(147,4)==148 && volume_step(1,-4)==0);
  assert(!volume_valid(-1) && !volume_valid(149) && !volume_valid(512));
  hotkey_t h={0}; int v=68;
  assert(hotkey(&h,PAD_MOD,0,v)==v);
  assert(hotkey(&h,PAD_MOD,499,v)==v);
  assert(hotkey(&h,PAD_MOD,500,v)==v);
  v=hotkey(&h,PAD_MOD|PAD_UP,501,v); assert(v==72);
  assert(hotkey(&h,PAD_MOD|PAD_UP,650,v)==v);
  v=hotkey(&h,PAD_MOD|PAD_UP,651,v); assert(v==76);
  v=hotkey(&h,PAD_MOD|PAD_LEFT,700,v); assert(v==(76|TTGK_MUTE));
  v=hotkey(&h,PAD_MOD|PAD_RIGHT,701,v); assert(v==76);
  assert(hotkey(&h,0,702,v)==v);
  assert(hotkey(&h,PAD_MOD|PAD_DOWN,703,v)==v);
  assert(hotkey(&h,PAD_MOD|PAD_DOWN,2000,v)==v);
  uint8_t malformed[]={9,2,255,255,0,1,0,0,0};
  assert(!ttgk_config(malformed,sizeof(malformed)));
  assert(pcm_word(NAN)==0 && pcm_word(INFINITY)==0x7fffffffu);
  assert(ttgk_config(ttgk_descriptor,sizeof(ttgk_descriptor)));
  for (size_t i=0;i<sizeof(ttgk_descriptor);i++)
    assert(!ttgk_config(ttgk_descriptor,i));
  unsigned char wrong[sizeof(ttgk_descriptor)];
  memcpy(wrong,ttgk_descriptor,sizeof(wrong)); wrong[1]=7;
  assert(!ttgk_config(wrong,sizeof(wrong)));
  memcpy(wrong,ttgk_descriptor,sizeof(wrong));
  for (size_t i=0;i<sizeof(wrong);i+=wrong[i]) {
    if (wrong[i+1]==5 && wrong[i+2]==1 && wrong[i+4]==0x20 && wrong[i+5]==1) {
      wrong[i+3]=5; break;
    }
  }
  assert(!ttgk_config(wrong,sizeof(wrong)));
  puts("portable tests passed");
}
