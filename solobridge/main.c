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
static capture_stats capture_snapshot(source_t *s){
  return (capture_stats){atomic_load(&s->reads),atomic_load(&s->empty),atomic_load(&s->frames),atomic_load(&s->gaps),atomic_load(&s->gap_sum_us),atomic_load(&s->read_min),atomic_load(&s->read_max),atomic_load(&s->gap_max_us)};
}
static void configure(source_t *s,eng_player *p,bench_profile q){
  atomic_store(&s->request_frames,q.request);atomic_store(&s->poll_us,q.poll);atomic_store(&s->adaptive,q.adaptive);
  eng_profile(p,q.request,q.reserve,q.trim);p->vector=q.vector;p->prime_reserve=q.prime;
}
static int thread_cpu(double *value){
#ifdef CLOCK_THREAD_CPUTIME_ID
  struct timespec t;if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0){*value=t.tv_sec+t.tv_nsec/1e9;return 1;}
#endif
  *value=0;return 0;
}
static double queue_ms(eng_ring *r,solo_t *s,int online,unsigned batch,eng_pacer *pace){
  return eng_fill(r)/48.0+(online&&batch?__builtin_popcount(s->fs.busy)*batch:pace->queued)/8.0;
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
  bench_profile profile=bench_modes[0];unsigned batch=0;capture_stats cap_start={0};double qnext=0;queue_stats qs={0};uint64_t source_cpu_start=0;double main_cpu_start=0;int main_cpu_valid=0,source_cpu_valid=0;
  int test=-1,measuring=0;unsigned selected=0,active_profile=0,rounds=2,bad_rate=0;
  uint64_t guard_packets=0,guard_underruns=0,guard_skipped=0,guard_drops=0,empty_wakes=0,timeouts=0,empty_start=0,timeout_start=0;
  double guard_at=0,guard_outage=0;int guard_ready=0;queue_stats ring_qs={0};
  uint64_t guard_overruns=0,phase_under=0,phase_skip=0,phase_drop=0,phase_over=0,phase_preroll=0;
  double head_sum=0,tail_sum=0;uint64_t head_n=0,tail_n=0;
  uint64_t perf_fs_base=0,perf_packets_base=0,perf_empty_base=0;double fs_rate=0,packet_rate=0,empty_rate=0;double phase_at=0,stable_at=0,fs_last=0;bench_sample start={0};
  int economy=1;
  if(access(DIR "/legacy_pacing",F_OK)==0)economy=0;
  web_set_economy(economy);
  perf_sample previous=perf_take(now_sec(),0,0,0);
  perf_rate rate={0}; double next_perf=previous.at+5;
  log_line("pacing mode=%s",economy?"economy":"legacy");
  while (running && !control_stop_requested()) {
    double now=now_sec(); loops++;
    int command=web_bench_command();
    if(command>=100&&profile_valid((unsigned)(command-100))&&test<0){
      selected=active_profile=(unsigned)(command-100);profile=bench_modes[selected];batch=profile.batch;
      if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
      next_open=now_sec();web_set_economy(1);previous=perf_take(next_open,writes,loops,sleeps);next_perf=next_open+5;rate=(perf_rate){0};perf_fs_base=fs_calls;perf_packets_base=packets;perf_empty_base=empty_wakes;fs_rate=packet_rate=empty_rate=0;log_line("selected playback profile=%u",selected);
    }else if((command==1||command==3)&&test<0){
      rounds=1;test=0;active_profile=bench_id(0);profile=bench_modes[active_profile];batch=profile.batch;measuring=0;phase_at=now;stable_at=0;
      phase_under=player.underruns;phase_skip=player.skipped;phase_drop=atomic_load(&src.dropped);phase_over=atomic_load(&src.overruns);phase_preroll=player.preroll;
      web_bench_begin();web_bench_line("rounds=%u profiles=%zu; warmup=%d measurement=%ds",rounds,BENCH_COUNT,BENCH_WARMUP,BENCH_SECONDS);web_set_economy(1);
      if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
      next_open=now_sec();
    } else if(command==2){
      if(test<0){web_bench_begin();web_bench_line("CANCELLED before startup");web_bench_end();}
      else {
      web_bench_line("CANCELLED; restoring selected profile");
      web_bench_end();test=-1;active_profile=selected;profile=bench_modes[selected];batch=profile.batch;
      if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
      next_open=now_sec();
      }
    }
    int mode=web_get_economy();
    if(mode!=economy){economy=mode; previous=perf_take(now,writes,loops,sleeps);rate=(perf_rate){0};next_perf=now+5;log_line("pacing mode=%s",economy?"economy":"legacy");}
    int failed=0,volume_failure=0;
    if (atomic_load(&src.fatal)) { result=1; break; }
    if (online && now-last_tick>0.25) { log_line("timing gap %.0fms; reopen USB",1000*(now-last_tick)); failed=1; }
    last_tick=now;
    if (!online && now>=next_open) {
      configure(&src,&player,profile);
      if (solo_open(&solo,1,control_volume(),batch,profile.slots)==0) {
        online=1; reconnects++; have=gate=0; ack=-1; ready_since=blocked_since=0;
        eng_reset(&src.ring,&player); eng_pace_init(&pace,now_sec());
        epoch=atomic_load(&src.epoch);
        source_enable(&src,1); control_connected(); last_tick=now_sec();fs_last=last_tick;guard_ready=0;bad_rate=0;guard_at=guard_outage=last_tick;guard_packets=packets;guard_underruns=player.underruns;guard_skipped=player.skipped;guard_drops=atomic_load(&src.dropped);guard_overruns=atomic_load(&src.overruns);
      } else {next_open=now_sec()+1;if(test>=0&&batch){web_bench_line("USB_FS setup failed; see log for ioctl and errno");failed=1;}else if(test<0&&selected){log_line("profile %u setup failed; fallback to write",selected);selected=active_profile=0;profile=bench_modes[0];batch=0;web_set_economy(1);}}
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
              if(profile.efficient==2){if(fs_drain(f,solo.ctl_fd)<0)failed=1;}
              else for(unsigned i=0;i<(profile.efficient?1u:f->opened);i++){int v=fs_complete(f,solo.ctl_fd);if(v<0){failed=1;break;}if(!v)break;}
              if(!failed&&f->completed==done_before){empty_wakes++;sleeps++;usleep(1000);}
            }else if(r==0){timeouts++;
            }
          }
        }
        packets+=f->completed-done_before;
        if(f->completed!=done_before)fs_last=now_sec();
        if(now_sec()-fs_last>0.15){log_line("USB_FS completion watchdog");failed=1;}
        for(unsigned i=0;i<f->opened&&!failed;i++)if(!(f->busy&(1u<<i))){
          if(cap&&profile.efficient)eng_packets(&src.ring,&player,(uint8_t *)&f->data[i],batch);
          else if(cap){for(unsigned j=0;j<batch;j++)eng_packet(&src.ring,&player,f->data[i][j]);}
          else memset(f->data[i],0,batch*ENG_BYTES);
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
      if (cap && source_active(&src) && atomic_load(&src.epoch)==epoch && player.playing && !failed && !blocked_since && (!batch||solo.fs.completed)) {
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
    /* Runtime guard only for explicitly selected USB_FS profiles. Benchmarks
     * must report failures, not silently change modes. */
    if(test<0&&selected&&online&&!failed){
      int healthy=gate&&source_active(&src);
      if(healthy){
        guard_outage=0;
        if(!guard_ready){guard_ready=1;guard_packets=packets;bad_rate=0;}
      }else{
        guard_ready=0;
        if(!guard_outage)guard_outage=now;
        if(now-guard_outage>15){log_line("selected profile startup/recovery timeout");failed=1;}
      }
      if(player.skipped!=guard_skipped||atomic_load(&src.dropped)!=guard_drops||
         atomic_load(&src.overruns)!=guard_overruns||player.underruns-guard_underruns>=2){
        log_line("profile %u guard: data loss or recurrent underrun; fallback",selected);failed=1;
      }
      if(now-guard_at>=5){
        if(guard_ready){
          double pr=(packets-guard_packets)/(now-guard_at);
          bad_rate=rate_bad(bad_rate,pr);
          if(bad_rate>=2){log_line("profile %u guard: packets/s=%.1f; fallback",selected,pr);failed=1;}
        }else bad_rate=0;
        guard_at=now;guard_packets=packets;guard_underruns=player.underruns;
        guard_skipped=player.skipped;guard_drops=atomic_load(&src.dropped);guard_overruns=atomic_load(&src.overruns);
      }
    }
    if (online && failed) {
      control_disconnected(volume_failure);
      source_enable(&src,0);
      solo_close(&solo); online=0;
      late_total+=pace.late; blocked_total+=pace.blocked;
      eng_reset(&src.ring,&player); next_open=now_sec()+1; last_tick=now_sec();
      if(test<0&&selected){log_line("profile %u failed; fallback to safe write",selected);selected=active_profile=0;profile=bench_modes[0];batch=0;web_set_economy(1);}
    }
    now=now_sec();
    if(test>=0){
      int finish=0;
      if(measuring&&now>=qnext){double q=queue_ms(&src.ring,&solo,online,batch,&pace);queue_add(&qs,q);queue_add(&ring_qs,eng_fill(&src.ring)/48.0);double elapsed=now-start.cpu.at;if(elapsed<5){head_sum+=q;head_n++;}if(elapsed>=BENCH_SECONDS-5){tail_sum+=q;tail_n++;}qnext=now+0.001;}
      bench_sample cur={.cpu={0},.packets=packets,.underruns=player.underruns,.skipped=player.skipped,.late=late_total+(online?pace.late:0),.blocked=blocked_total+(online?pace.blocked:0),.drop=atomic_load(&src.dropped),.overruns=atomic_load(&src.overruns),.reconnects=reconnects,.fs_calls=fs_calls,.epoch=atomic_load(&src.epoch)};
      if(failed){web_bench_line("mode=%u FAIL stream/setup error",batch);finish=1;}
      else if(!measuring){
        if(online&&gate&&source_active(&src)&&solo_volume_done(&solo)>=0){
          if(!stable_at)stable_at=now;
          if(now-stable_at>=BENCH_WARMUP){cur.cpu=perf_take(now,writes,loops,sleeps);start=cur;cap_start=capture_snapshot(&src);main_cpu_valid=thread_cpu(&main_cpu_start);source_cpu_start=atomic_load(&src.cpu_ns);source_cpu_valid=atomic_load(&src.cpu_valid);qs=(queue_stats){0};ring_qs=(queue_stats){0};empty_start=empty_wakes;timeout_start=timeouts;head_sum=tail_sum=0;head_n=tail_n=0;qnext=now;measuring=1;web_bench_line("round=%u profile=%u batch=%u slots=%u request=%u reserve=%u prime=%u poll=%u adaptive=%u simd=%u completion=%u measuring %ds",(unsigned)test/(unsigned)BENCH_COUNT+1,active_profile,batch,profile.slots,profile.request,profile.reserve,profile.prime,profile.poll,profile.adaptive,profile.vector,profile.efficient,BENCH_SECONDS);}
        }else stable_at=0;
        if(now-phase_at>15&&!measuring){web_bench_line("mode=%u FAIL startup timeout",batch);finish=1;}
      }else if(!online||!gate||cur.epoch!=start.epoch){
        web_bench_line("mode=%u FAIL capture/gate changed",batch);finish=1;
      }else if(now-start.cpu.at>=BENCH_SECONDS){
        cur.cpu=perf_take(now,writes,loops,sleeps);perf_rate r=perf_diff(start.cpu,cur.cpu);
        int ok=bench_pass(start,cur,batch)&&r.valid;
        double growth=head_n&&tail_n?tail_sum/tail_n-head_sum/head_n:0;
        if(!head_n||!tail_n||growth>4.0)ok=0;
        uint64_t warm_under=start.underruns-phase_under,warm_skip=start.skipped-phase_skip,warm_drop=start.drop-phase_drop,warm_over=start.overruns-phase_over;
        int warm_ok=!warm_under&&!warm_skip&&!warm_drop&&!warm_over;
        double main_cpu=0;int mv=thread_cpu(&main_cpu)&&main_cpu_valid;uint64_t sc=atomic_load(&src.cpu_ns);int sv=source_cpu_valid&&atomic_load(&src.cpu_valid)&&sc>=source_cpu_start;
        web_bench_line("profile=%u writer_cpu_valid=%d writer_cpu=%.3f source_cpu_valid=%d source_cpu=%.3f preroll_discard_total=%llu",active_profile,mv,mv?100*(main_cpu-main_cpu_start)/r.seconds:0,sv,sv?100*(sc-source_cpu_start)/1e9/r.seconds:0,(unsigned long long)player.preroll);
        capture_stats cs=capture_snapshot(&src);double capture_rate=(cs.frames-cap_start.frames)/r.seconds;
        if(capture_rate<47000||capture_rate>49000)ok=0;
        double cg=cs.gaps>cap_start.gaps?(cs.gap_us-cap_start.gap_us)/(double)(cs.gaps-cap_start.gaps)/1000:0;
        web_bench_line("profile=%u capture_frames_min=%u max=%u mean_gap_ms=%.3f session_max_gap_ms=%.3f reads/s=%.1f empty/s=%.1f capture_frames/s=%.1f queue_sample_mean_ms=%.3f queue_sample_p95_upper_ms=%.3f queue_sample_max_ms=%.3f samples=%llu",active_profile,cs.min,cs.max,cg,cs.gap_max/1000.0,(cs.reads-cap_start.reads)/r.seconds,(cs.empty-cap_start.empty)/r.seconds,capture_rate,qs.n?qs.sum/qs.n:0,queue_p95(&qs),qs.max,(unsigned long long)qs.n);
        web_bench_line("profile=%u startup_underrun=%llu startup_skip=%llu startup_drop=%llu startup_overrun=%llu preroll_this_phase=%llu actual_reserve=%u queue_growth_ms=%.3f",active_profile,(unsigned long long)warm_under,(unsigned long long)warm_skip,(unsigned long long)warm_drop,(unsigned long long)warm_over,(unsigned long long)(player.preroll-phase_preroll),player.target,growth);
        web_bench_line("profile=%u ring_mean_ms=%.3f ring_p95_upper_ms=%.3f ring_max_ms=%.3f empty_wakes/s=%.1f select_timeouts/s=%.1f usb_inflight_upper_ms=%.3f",active_profile,ring_qs.n?ring_qs.sum/ring_qs.n:0,queue_p95(&ring_qs),ring_qs.max,(empty_wakes-empty_start)/r.seconds,(timeouts-timeout_start)/r.seconds,batch?batch*profile.slots/8.0:6.0);
        web_bench_line("round=%u profile=%u mode=%u %s seconds=%.3f cpu_valid=%d cpu=%.3f user=%.3f sys=%.3f writes/s=%.1f fs_calls/s=%.1f loops/s=%.1f sleep/s=%.1f packets/s=%.1f underrun=%llu skip=%llu late=%llu blocked=%llu drop=%llu overrun=%llu",(unsigned)test/(unsigned)BENCH_COUNT+1,active_profile,batch,ok?(warm_ok?"PASS_COUNTERS":"PASS_STEADY_WARN_STARTUP"):"FAIL_COUNTERS",r.seconds,r.valid,r.user_pct+r.sys_pct,r.user_pct,r.sys_pct,r.write_rate,(cur.fs_calls-start.fs_calls)/r.seconds,r.loop_rate,r.sleep_rate,(cur.packets-start.packets)/r.seconds,(unsigned long long)(cur.underruns-start.underruns),(unsigned long long)(cur.skipped-start.skipped),(unsigned long long)(cur.late-start.late),(unsigned long long)(cur.blocked-start.blocked),(unsigned long long)(cur.drop-start.drop),(unsigned long long)(cur.overruns-start.overruns));finish=1;
      }
      if(finish){
        if(online){control_disconnected(0);source_enable(&src,0);solo_close(&solo);online=0;late_total+=pace.late;blocked_total+=pace.blocked;}
        test++;measuring=0;stable_at=0;phase_at=now_sec();next_open=phase_at;
        phase_under=player.underruns;phase_skip=player.skipped;phase_drop=atomic_load(&src.dropped);phase_over=atomic_load(&src.overruns);phase_preroll=player.preroll;
        if((unsigned)test>=rounds*BENCH_COUNT){test=-1;active_profile=selected;profile=bench_modes[selected];batch=profile.batch;web_bench_line("FINISHED; restoring selected profile=%u. Confirm sound manually.",selected);web_bench_end();}
        else {active_profile=bench_id((unsigned)test);profile=bench_modes[active_profile];batch=profile.batch;web_bench_line("round=%u profile=%u batch=%u slots=%u request=%u reserve=%u prime=%u poll=%u adaptive=%u simd=%u completion=%u starting",(unsigned)test/(unsigned)BENCH_COUNT+1,active_profile,batch,profile.slots,profile.request,profile.reserve,profile.prime,profile.poll,profile.adaptive,profile.vector,profile.efficient);}
        previous=perf_take(now_sec(),writes,loops,sleeps);rate=(perf_rate){0};next_perf=previous.at+5;perf_fs_base=fs_calls;perf_packets_base=packets;perf_empty_base=empty_wakes;fs_rate=packet_rate=empty_rate=0;
      }
    }
    if(now>=next_perf){perf_sample sample=perf_take(now,writes,loops,sleeps);rate=perf_diff(previous,sample);if(rate.seconds>0){fs_rate=(fs_calls-perf_fs_base)/rate.seconds;packet_rate=(packets-perf_packets_base)/rate.seconds;empty_rate=(empty_wakes-perf_empty_base)/rate.seconds;}perf_fs_base=fs_calls;perf_packets_base=packets;perf_empty_base=empty_wakes;previous=sample;next_perf=now+5;}
    if (web_snapshot_requested()) {
      web_audio s={online,source_active(&src),player.playing,online&&gate,online?solo_volume_raw(&solo):-1,(int)eng_fill(&src.ring),player.delta*1000000/ENG_Q,(online&&batch?(double)(__builtin_popcount(solo.fs.busy)*batch):pace.queued),player.underruns,late_total+(online?pace.late:0),blocked_total+(online?pace.blocked:0),reconnects,atomic_load(&src.dropped),player.skipped,writes,rate,batch,profile.slots,atomic_load(&src.read_min),atomic_load(&src.read_max),atomic_load(&src.gap_max_us),active_profile,selected,player.target,fs_rate,packet_rate,empty_rate};
      web_publish(&s);
    }
    if (now>=next_stats) {
      log_line("status usb=%d capture=%d ring=%zu frames target=%u phase_ppm=%d queue_est=%.1f packets underruns=%llu skipped=%llu late=%llu blocked=%llu source_drop=%lu source_overruns=%lu reconnects=%llu",
        online,source_active(&src),eng_fill(&src.ring),player.target,player.delta*1000000/ENG_Q,(online&&batch?(double)(__builtin_popcount(solo.fs.busy)*batch):pace.queued),
        (unsigned long long)player.underruns,(unsigned long long)player.skipped,
        (unsigned long long)(late_total+(online?pace.late:0)),(unsigned long long)(blocked_total+(online?pace.blocked:0)),
        atomic_load(&src.dropped),atomic_load(&src.overruns),(unsigned long long)reconnects);
      log_line("perf profile=%u mode=%s cpu_valid=%d cpu_user=%.2f%% cpu_sys=%.2f%% write/s=%.1f fs_calls/s=%.1f packets/s=%.1f empty_wake/s=%.1f loops/s=%.1f sleeps/s=%.1f",active_profile,batch?"USB_FS":economy?"economy":"legacy",rate.valid,rate.user_pct,rate.sys_pct,rate.write_rate,fs_rate,packet_rate,empty_rate,rate.loop_rate,rate.sleep_rate);
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
