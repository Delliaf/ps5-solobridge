#include <assert.h>
#include <stdio.h>
#include "../solobridge/volume_logic.h"
typedef struct { int trace[16],count,fail,allow; } mock;
static int record(mock *m,int what) { m->trace[m->count++]=what; return m->count==m->fail?-1:0; }
static int mute(void *p,int v) { return record(p,v?10:11); }
static int channel(void *p,int c,int units) { assert(units<=0 && units>=-18944 && units%128==0); return record(p,c); }
static int allow(void *p) { return ((mock*)p)->allow; }
int main(void) {
  hw_volume_ops o={mute,channel,allow}; hw_volume_state s={-1,-1}; mock m={.allow=1};
  assert(hw_volume_apply(&s,&o,&m,68|TTGK_MUTE)==0);
  assert(m.count==3 && m.trace[0]==10 && m.trace[1]==1 && m.trace[2]==2);
  assert(s.mute==1 && s.volume==68);
  m.count=0; m.allow=0;
  assert(hw_volume_apply(&s,&o,&m,72)==1 && s.mute==1 && s.volume==72);
  assert(m.count==2);
  m.count=0; m.allow=1;
  assert(hw_volume_apply(&s,&o,&m,72)==0 && m.count==1 && m.trace[0]==11);
  m.count=0; assert(hw_volume_apply(&s,&o,&m,72)==0 && !m.count);
  assert(hw_volume_apply(&s,&o,&m,76)==0 && m.count==2);
  m.count=0;m.fail=2;
  assert(hw_volume_apply(&s,&o,&m,80)<0 && s.volume==-1 && s.mute==-1);
  m.count=0;m.fail=0;
  assert(hw_volume_apply(&s,&o,&m,76|TTGK_MUTE)==0 && m.trace[0]==10);
  puts("hardware volume transaction tests passed");
}
