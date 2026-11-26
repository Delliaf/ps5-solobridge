#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../solobridge/probe_logic.h"
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int main(void) {
  assert(probe_duration("off_a")==8 && probe_duration("measurement_on_b")==8);
  assert(probe_duration("calibration")==20 && !probe_duration("../a") && !probe_duration("off_a.wav"));
  assert(!probe_duration("on_a\n") && !probe_duration(""));
  uint8_t h[44]; probe_wav_header(h,48000*8);
  assert(!memcmp(h,"RIFF",4) && !memcmp(h+8,"WAVEfmt ",8));
  assert(le32(h+4)==36+48000*8*4 && le32(h+40)==48000*8*4);
  assert(le32(h+24)==48000 && le32(h+28)==192000 && h[22]==2 && h[34]==16);
  float x[]={1,-1,0.5f,-0.5f,NAN,INFINITY};
  uint8_t out[12]; probe_pcm(out,x,3);
  const uint8_t expected[]={0xff,0x7f,0,0x80,0,0x40,0,0xc0,0,0,0xff,0x7f};
  assert(!memcmp(out,expected,12));
  puts("Tempest probe format and request tests passed");
}
