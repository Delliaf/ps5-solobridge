/* GPL-3.0. TTGK bridge: isolated AVCAP, USB control, local input and logs. */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/select.h>
#include "bench.h"
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
  uint64_t reconnects=0,late_total=0,blocked_total=0,writes=0,loops=0,sleeps=0,packets=0,fs_calls=0;
  unsigned batch=0;int test=-1,measuring=0;double phase_at=0,stable_at=0,fs_last=0;bench_sample start={0};
  int economy=1;
  if(access(DIR "/legacy_pacing",F_OK)==0)economy=0;
  web_set_economy(economy);
  perf_sample previous=perf_take(now_sec(),0,0,0);
  perf_rate rate={0}; double next_perf=previous.at+5;
  log_line("pacing mode=%s",economy?"economy":"legacy");
  while (running && !control_stop_requested()) {
    double now=now_sec(); loops++;
    int command=web_bench_command();
    if(command==1&&test<0){
      test=0;batch=0;measuring=0;phase_at=now;stable_at=0;
      web_bench_begin();web_set_economy(1);
      if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
      next_open=now_sec();
    } else if(command==2&&test>=0){
      web_bench_line("CANCELLED; restoring normal USB write");
      web_bench_end();test=-1;batch=0;
      if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
      next_open=now_sec();
    }
    int mode=web_get_economy();
    if(mode!=economy){economy=mode; previous=perf_take(now,writes,loops,sleeps);rate=(perf_rate){0};next_perf=now+5;log_line("pacing mode=%s",economy?"economy":"legacy");}
    int failed=0,volume_failure=0;
    if (atomic_load(&src.fatal)) { result=1; break; }
    if (online && now-last_tick>0.25) { log_line("timing gap %.0fms; reopen USB",1000*(now-last_tick)); failed=1; }
    last_tick=now;
    if (!online && now>=next_open) {
      if (solo_open(&solo,1,control_volume(),batch)==0) {
        online=1; reconnects++; have=gate=0; ack=-1; ready_since=blocked_since=0;
        eng_reset(&src.ring,&player); eng_pace_init(&pace,now_sec());
        epoch=atomic_load(&src.epoch);
        source_enable(&src,1); control_connected(); last_tick=now_sec();fs_last=last_tick;
      } else {next_open=now_sec()+1;if(test>=0&&batch){web_bench_line("USB_FS setup failed; see log for ioctl and errno");failed=1;}}
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
      int written=0;
      if(batch){
        /* The kernel schedules the individual microframes. Never combine
         * frames into one write; each slot has an explicit length array. */
        usbfs *f=&solo.fs;uint64_t prior=f->calls,done_before=f->completed;
        if(f->busy){
          if(solo.ctl_fd>=FD_SETSIZE){errno=EINVAL;failed=1;}
          else {
            fd_set rd;FD_ZERO(&rd);FD_SET(solo.ctl_fd,&rd);
            struct timeval tv={.tv_sec=0,.tv_usec=10000};
            sleeps++;int r=select(solo.ctl_fd+1,&rd,NULL,NULL,&tv);fs_calls++;
            if(r<0&&errno!=EINTR)failed=1;
            if(r>0){
              for(unsigned i=0;i<FS_SLOTS;i++){int v=fs_complete(f,solo.ctl_fd);if(v<0){failed=1;break;}if(!v)break;}
              if(!failed&&f->completed==done_before)usleep(1000);
            }
          }
        }
        packets+=f->completed-done_before;
        if(f->completed!=done_before)fs_last=now_sec();
        if(now_sec()-fs_last>0.15){log_line("USB_FS completion watchdog");failed=1;}
        for(unsigned i=0;i<FS_SLOTS&&!failed;i++)if(!(f->busy&(1u<<i))){
          for(unsigned j=0;j<batch;j++){
            if(cap)eng_packet(&src.ring,&player,f->data[i][j]);
            else memset(f->data[i][j],0,ENG_BYTES);
          }
          if(fs_submit(f,solo.ctl_fd,i)<0){log_line("USB_FS START errno=%d",errno);failed=1;break;}
        }
        fs_calls+=f->calls-prior;
        if(failed)log_line("USB_FS stream failure batch=%u errno=%d",batch,errno);
      }else{
      eng_pace_tick(&pace,now_sec());
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
        have=0; blocked_since=0; eng_pace_write(&pace); written++;packets++;
      }
      }
      /* Keep hardware muted until capture is prebuffered and packets accepted.
       * Gate is also removed on session loss and underrun. */
      double t=now_sec();
      if (cap && player.playing && !failed && !blocked_since && (!batch||solo.fs.completed)) {
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
      if(!failed&&!batch) {
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
    if(test>=0){
      int finish=0;
      bench_sample cur={.cpu={0},.packets=packets,.underruns=player.underruns,.skipped=player.skipped,.late=late_total+(online?pace.late:0),.blocked=blocked_total+(online?pace.blocked:0),.drop=atomic_load(&src.dropped),.overruns=atomic_load(&src.overruns),.reconnects=reconnects,.fs_calls=fs_calls,.epoch=atomic_load(&src.epoch)};
      if(failed){web_bench_line("mode=%u FAIL stream/setup error",batch);finish=1;}
      else if(!measuring){
        if(online&&gate&&source_active(&src)){
          if(!stable_at)stable_at=now;
          if(now-stable_at>=5){cur.cpu=perf_take(now,writes,loops,sleeps);start=cur;measuring=1;web_bench_line("mode=%u measuring 25s",batch);}
        }else stable_at=0;
        if(now-phase_at>15&&!measuring){web_bench_line("mode=%u FAIL startup timeout",batch);finish=1;}
      }else if(!online||!gate||cur.epoch!=start.epoch){
        web_bench_line("mode=%u FAIL capture/gate changed",batch);finish=1;
      }else if(now-start.cpu.at>=25){
        cur.cpu=perf_take(now,writes,loops,sleeps);perf_rate r=perf_diff(start.cpu,cur.cpu);
        int ok=bench_pass(start,cur,batch);
        web_bench_line("mode=%u %s seconds=%.3f cpu_valid=%d cpu=%.3f user=%.3f sys=%.3f writes/s=%.1f fs_calls/s=%.1f loops/s=%.1f sleep/s=%.1f packets/s=%.1f underrun=%llu skip=%llu late=%llu blocked=%llu drop=%llu overrun=%llu",batch,ok?"PASS_COUNTERS":"FAIL_COUNTERS",r.seconds,r.valid,r.user_pct+r.sys_pct,r.user_pct,r.sys_pct,r.write_rate,(cur.fs_calls-start.fs_calls)/r.seconds,r.loop_rate,r.sleep_rate,(cur.packets-start.packets)/r.seconds,(unsigned long long)(cur.underruns-start.underruns),(unsigned long long)(cur.skipped-start.skipped),(unsigned long long)(cur.late-start.late),(unsigned long long)(cur.blocked-start.blocked),(unsigned long long)(cur.drop-start.drop),(unsigned long long)(cur.overruns-start.overruns));finish=1;
      }
      if(finish){
        if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
        test++;measuring=0;stable_at=0;phase_at=now_sec();next_open=phase_at;
        if(test>=BENCH_MODES){test=-1;batch=0;web_bench_line("FINISHED; restored normal USB write. Confirm sound manually.");web_bench_end();}
        else {batch=bench_modes[test];web_bench_line("mode=%u starting",batch);}
        previous=perf_take(now_sec(),writes,loops,sleeps);rate=(perf_rate){0};next_perf=previous.at+5;
      }
    }
    if(now>=next_perf){perf_sample sample=perf_take(now,writes,loops,sleeps);rate=perf_diff(previous,sample);previous=sample;next_perf=now+5;}
    if (web_snapshot_requested()) {
      web_audio s={online,source_active(&src),player.playing,online&&gate,online?solo_volume_raw(&solo):-1,(int)eng_fill(&src.ring),player.delta*1000000/ENG_Q,pace.queued,player.underruns,late_total+(online?pace.late:0),blocked_total+(online?pace.blocked:0),reconnects,atomic_load(&src.dropped),player.skipped,writes,rate,batch};
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
  if(test>=0){web_bench_line("ABORTED by payload shutdown");web_bench_end();}
  web_stop();
  control_disconnected(0); source_enable(&src,0);
  if (online) solo_close(&solo);
  source_stop(&src); probe_stop(); control_stop(); log_line("TTGK stopped result=%d",result);
  log_stop(); close(lock); return result;
}
