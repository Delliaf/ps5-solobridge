/* GPL-3.0. TTGK bridge: isolated AVCAP, USB control, local input and logs. */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "perf.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "solo.h"
#include "control.h"
#include "source.h"
#include "engine.h"
#include "log.h"
#include "probe.h"
#include "web.h"
#include "version.h"
#define DIR "/data/solobridge"
static volatile sig_atomic_t running=1;
static void stop_signal(int s) { (void)s; running=0; }
static double now_sec(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return t.tv_sec+t.tv_nsec/1e9;
}
static perf_sample perf_take(double now,uint64_t writes,uint64_t loops,uint64_t sleeps) {
  perf_sample p={.at=now,.writes=writes,.loops=loops,.sleeps=sleeps};
  struct rusage ru;
  if(getrusage(RUSAGE_SELF,&ru)==0){
    p.valid=1;p.user=ru.ru_utime.tv_sec+ru.ru_utime.tv_usec/1e6;
    p.sys=ru.ru_stime.tv_sec+ru.ru_stime.tv_usec/1e6;
  }
  return p;
}
int main(void) {
  signal(SIGPIPE,SIG_IGN); signal(SIGINT,stop_signal); signal(SIGTERM,stop_signal);
  umask(0077); mkdir(DIR,0700);
  int lock=open(DIR "/instance.lock",O_CREAT|O_RDWR,0600);
  if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)<0) {
    if (lock>=0) close(lock);
    fprintf(stderr,"TTGK: already running or cannot lock instance\n"); return 1;
  }
  if (log_start()<0) { close(lock); return 1; }
  log_line("TTGK Audio Bridge v%s: started pid=%d",TTGK_VERSION,getpid());
  FILE *vf=fopen(DIR "/version.txt","w");
  if(vf){fprintf(vf,"%s pid=%d\n",TTGK_VERSION,getpid());fclose(vf);}
  if (control_start()<0) { log_line("control worker failed"); log_stop(); close(lock); return 1; }
  if (web_start()<0) log_line("Web panel unavailable; audio continues");
  int probe=probe_start();
  if (probe<0) log_line("Tempest probe init failed; normal bridge remains available");
  else if (probe) log_line("Tempest diagnostic enabled (explicit source capture only)");
  static source_t src;
  if (source_start(&src)<0) { log_line("capture worker failed"); probe_stop(); web_stop(); control_stop(); log_stop(); close(lock); return 1; }
  solo_t solo; int online=0,result=0,have=0,gate=0,ack=-1;
  double next_open=0,next_stats=now_sec()+30,blocked_since=0,ready_since=0,last_tick=now_sec();
  unsigned epoch=0;

  eng_player player; eng_player_init(&player);
  eng_pacer pace; eng_pace_init(&pace,last_tick);
  uint8_t packet[ENG_BYTES];
  uint64_t reconnects=0,late_total=0,blocked_total=0,writes=0,loops=0,sleeps=0;
  int economy=1;
  if(access(DIR "/legacy_pacing",F_OK)==0)economy=0;
  web_set_economy(economy);
  perf_sample previous=perf_take(now_sec(),0,0,0);
  perf_rate rate={0}; double next_perf=previous.at+5;
  log_line("pacing mode=%s",economy?"economy":"legacy");
  while (running && !control_stop_requested()) {
    double now=now_sec(); loops++;
    int mode=web_get_economy();
    if(mode!=economy){economy=mode; previous=perf_take(now,writes,loops,sleeps);rate=(perf_rate){0};next_perf=now+5;log_line("pacing mode=%s",economy?"economy":"legacy");}
    int failed=0,volume_failure=0;
    if (atomic_load(&src.fatal)) { result=1; break; }
    if (online && now-last_tick>0.25) { log_line("timing gap %.0fms; reopen USB",1000*(now-last_tick)); failed=1; }
    last_tick=now;
    if (!online && now>=next_open) {
      if (solo_open(&solo,1,control_volume())==0) {
        online=1; reconnects++; have=gate=0; ack=-1; ready_since=blocked_since=0;
        eng_reset(&src.ring,&player); eng_pace_init(&pace,now_sec());
        epoch=atomic_load(&src.epoch);
        source_enable(&src,1); control_connected(); last_tick=now_sec();
      } else next_open=now_sec()+1;
    }
    if (online && !failed && solo_volume_error(&solo)) {
      log_line("USB control failed or blocked >500ms"); failed=volume_failure=1;
    }
    if (online && !failed) {
      unsigned current=atomic_load(&src.epoch);
      if (current!=epoch) {
        epoch=current; eng_reset(&src.ring,&player);
        have=gate=0; ready_since=0;
      }
      int cap=source_active(&src);
      if (!cap) {
        if (player.playing || have || eng_fill(&src.ring)) { eng_reset(&src.ring,&player); have=0; }
        gate=0; ready_since=0;
      }
      eng_pace_tick(&pace,now_sec());
      int written=0;
      for (int burst=0;burst<96 && eng_pace_need(&pace);burst++) {
        if (!have) {
          if (cap) eng_packet(&src.ring,&player,packet);
          else { for (unsigned i=0;i<ENG_BYTES;i++) packet[i]=0; }
          have=1;
        }
        writes++;
        int w=solo_write(&solo,packet,sizeof(packet));
        if (w<0) { log_line("USB write failed errno=%d",errno); failed=1; break; }
        if (!w) {
          eng_pace_block(&pace);
          if (!blocked_since) blocked_since=now_sec();
          if (now_sec()-blocked_since>0.25) { log_line("USB FIFO blocked >250ms"); failed=1; }
          break;
        }
        have=0; blocked_since=0; eng_pace_write(&pace); written++;
      }
      /* Keep hardware muted until capture is prebuffered and packets accepted.
       * Gate is also removed on session loss and underrun. */
      double t=now_sec();
      if (cap && player.playing && !failed && !blocked_since) {
        if (!ready_since) ready_since=t;
        if (t-ready_since>=0.004) gate=1;
      } else { ready_since=0; gate=0; }
      solo_request_volume(&solo,control_volume(),gate);
      if (solo_volume_error(&solo)) { log_line("USB control failed or blocked >500ms"); failed=1; volume_failure=1; }
      int done=solo_volume_done(&solo);
      if (!volume_failure && done>=0 && done!=ack) {
        ack=done; control_applied(done);
        log_line("volume verified: %.1f dB mute=%d",(done&255)*0.5-74.0,!!(done&TTGK_MUTE));
      }
      if(!failed) {
        unsigned delay=0;
        if(blocked_since)delay=250;
        else if(economy)delay=eng_pace_sleep(&pace,now_sec(),1);
        else if(!written)delay=250;
        if(delay){sleeps++;usleep(delay);}
      }
    }
    if (online && failed) {
      control_disconnected(volume_failure);
      source_enable(&src,0);
      solo_close(&solo); online=0;
      late_total+=pace.late; blocked_total+=pace.blocked;
      eng_reset(&src.ring,&player); next_open=now_sec()+1; last_tick=now_sec();
    }
    now=now_sec();
    if(now>=next_perf){perf_sample sample=perf_take(now,writes,loops,sleeps);rate=perf_diff(previous,sample);previous=sample;next_perf=now+5;}
    if (web_snapshot_requested()) {
      web_audio s={online,source_active(&src),player.playing,online&&gate,online?solo_volume_raw(&solo):-1,(int)eng_fill(&src.ring),player.delta*1000000/ENG_Q,pace.queued,player.underruns,late_total+(online?pace.late:0),blocked_total+(online?pace.blocked:0),reconnects,atomic_load(&src.dropped),player.skipped,writes,rate};
      web_publish(&s);
    }
    if (now>=next_stats) {
      log_line("status usb=%d capture=%d ring=%zu frames target=%u phase_ppm=%d queue_est=%.1f packets underruns=%llu skipped=%llu late=%llu blocked=%llu source_drop=%lu source_overruns=%lu reconnects=%llu",
        online,source_active(&src),eng_fill(&src.ring),player.target,player.delta*1000000/ENG_Q,pace.queued,
        (unsigned long long)player.underruns,(unsigned long long)player.skipped,
        (unsigned long long)(late_total+(online?pace.late:0)),(unsigned long long)(blocked_total+(online?pace.blocked:0)),
        atomic_load(&src.dropped),atomic_load(&src.overruns),(unsigned long long)reconnects);
      log_line("perf mode=%s cpu_valid=%d cpu_user=%.2f%% cpu_sys=%.2f%% write/s=%.1f loops/s=%.1f sleeps/s=%.1f",economy?"economy":"legacy",rate.valid,rate.user_pct,rate.sys_pct,rate.write_rate,rate.loop_rate,rate.sleep_rate);
      next_stats=now+30;
    }
    if (!online) usleep(50000);
  }
  web_stop();
  control_disconnected(0); source_enable(&src,0);
  if (online) solo_close(&solo);
  source_stop(&src); probe_stop(); control_stop(); log_line("TTGK stopped result=%d",result);
  log_stop(); close(lock); return result;
}
