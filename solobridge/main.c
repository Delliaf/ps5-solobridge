// TTGK audio bridge. GPL-3.0; based on ps5-solobridge.
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "solo.h"
#include "control.h"
#include "bridge_logic.h"

#define LOG_DIR     "/data/solobridge"
#define LOG_PATH    LOG_DIR "/log.txt"

#define PKTS_PER_SEC   8000                // microframe USB high-speed
#define FRAMES_PER_PKT (SOLO_RATE / PKTS_PER_SEC)  // 6 frame ogni 125us
#define RING_FRAMES    16384               // ~340 ms
#define REC_FRAMES     1024                // la cattura arriva a blocchi da ~21 ms

// Il ring si riempie a scatti di REC_FRAMES; conta il livello minimo raggiunto
// (subito prima del blocco successivo): lo si tiene appena sopra lo zero.
#define MARGIN_TARGET  192                 // ~4 ms
#define MARGIN_LOW     48
#define MARGIN_HIGH    480
#define PREBUFFER      (REC_FRAMES + MARGIN_TARGET)
#define WINDOW_PKTS    4000                // finestra di misura: 0,5 s
// Al massimo 1 frame tolto/ripetuto ogni 64 pacchetti (8 ms): variazione di
// velocita' dello 0,26%, non percepibile come cambio di intonazione.
#define CORRECT_EVERY  64
// Oltre questa soglia (es. arretrato consegnato dalla cattura all'avvio) si
// scarta subito l'eccesso: correggerlo lentamente alzerebbe il pitch per secondi.
#define JUMP_FILL      4096                // ~85 ms

// Pacchetti tenuti in coda nel FIFO USB oltre a quello in trasmissione.
#define USB_AHEAD      48                  // ~6 ms

#define STATS_EVERY    60.0

typedef struct { float l, r; } frame_t;

static frame_t g_ring[RING_FRAMES];
static size_t g_head, g_fill;      // g_head = prossimo frame da leggere

static struct {
  long packets, silent, drops, dups, underruns, solo_reconnects, cap_restarts;
  long usb_late;                   // il loop e' arrivato tardi: FIFO USB vuoto
  long jumps, jumped;              // salti immediati e frame scartati con essi
  int fifo_depth;                  // pacchetti accettati dal FIFO USB a vuoto
  uint32_t solo_rate;              // sample rate riletto dalla Solo
  double usb_queued;               // stima pacchetti in coda
} st;

static struct {
  size_t win_min;
  long win_count;
  long pending_drop, pending_dup;
} drift = { (size_t)-1, 0, 0, 0 };

static FILE *g_log;
static volatile sig_atomic_t g_running = 1;
static void stop_signal(int sig) { (void)sig; g_running=0; }
static void reset_ring(void) { g_head=g_fill=0; drift.win_min=(size_t)-1; drift.win_count=0; drift.pending_drop=drift.pending_dup=0; }

// --- log -------------------------------------------------------------------

static void log_line(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[16];
  strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

  printf("[%s] %s\n", ts, msg);
  fflush(stdout);
  if (g_log) {
    fprintf(g_log, "[%s] %s\n", ts, msg);
    fflush(g_log);
  }
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// --- ring buffer -----------------------------------------------------------

static void ring_push(const float *in, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    if (g_fill == RING_FRAMES) {   // pieno: scarta il frame piu' vecchio
      g_head = (g_head + 1) % RING_FRAMES;
      g_fill--;
    }
    size_t tail = (g_head + g_fill) % RING_FRAMES;
    g_ring[tail].l = in[2 * i];
    g_ring[tail].r = in[2 * i + 1];
    g_fill++;
  }
}

static frame_t ring_pop(void) {
  frame_t f = g_ring[g_head];
  g_head = (g_head + 1) % RING_FRAMES;
  g_fill--;
  return f;
}

// --- conversione -----------------------------------------------------------

static void put_sample(uint8_t **p, float v, int subslot) {
  uint32_t s = pcm_word(v);
  if (subslot < 4)
    s >>= (4 - subslot) * 8;
  for (int b = 0; b < subslot; b++)
    *(*p)++ = s >> (8 * b);
}

static size_t silent_packet(uint8_t *pkt, const solo_t *s) {
  uint8_t *p = pkt;
  for (int f = 0; f < FRAMES_PER_PKT; f++)
    for (int c = 0; c < s->channels; c++)
      put_sample(&p, 0.0f, s->subslot);
  return p - pkt;
}

// Ogni WINDOW_PKTS pacchetti decide quanti frame togliere (ritardo in eccesso)
// o ripetere (margine troppo basso) in base al livello minimo del ring.
static void drift_update(void) {
  if (g_fill < drift.win_min)
    drift.win_min = g_fill;
  if (++drift.win_count < WINDOW_PKTS)
    return;
  drift.pending_drop = drift.pending_dup = 0;
  if (drift.win_min > MARGIN_HIGH)
    drift.pending_drop = drift.win_min - MARGIN_TARGET;
  else if (drift.win_min < MARGIN_LOW)
    drift.pending_dup = MARGIN_TARGET - drift.win_min;
  drift.win_min = (size_t)-1;
  drift.win_count = 0;
}

// Prepara il prossimo pacchetto. Gestisce prebuffer, underrun e deriva tra
// il clock dell'audio PS5 e quello dell'USB togliendo o ripetendo un frame.
static size_t build_packet(uint8_t *pkt, const solo_t *s, int *playing) {
  uint8_t *p = pkt;

  if (!*playing && g_fill >= PREBUFFER) {
    *playing = 1;
    drift.win_min = (size_t)-1;
    drift.win_count = 0;
    drift.pending_drop = drift.pending_dup = 0;
  } else if (*playing && g_fill < FRAMES_PER_PKT) {
    *playing = 0;
    st.underruns++;
  }
  if (!*playing) {
    st.silent++;
    return silent_packet(pkt, s);
  }

  if (g_fill > JUMP_FILL) {
    size_t skip = g_fill - PREBUFFER;
    g_head = (g_head + skip) % RING_FRAMES;
    g_fill -= skip;
    st.jumps++;
    st.jumped += skip;
    drift.win_min = (size_t)-1;
    drift.win_count = 0;
    drift.pending_drop = drift.pending_dup = 0;
  }

  drift_update();
  int dup = 0;
  if (st.packets % CORRECT_EVERY == 0) {
    if (drift.pending_drop > 0 && g_fill > 2 * FRAMES_PER_PKT) {
      ring_pop();
      drift.pending_drop--;
      st.drops++;
    } else if (drift.pending_dup > 0) {
      dup = 1;
      drift.pending_dup--;
      st.dups++;
    }
  }

  frame_t f = {0, 0};
  for (int i = 0; i < FRAMES_PER_PKT; i++) {
    if (!(dup && i == FRAMES_PER_PKT - 1))
      f = ring_pop();
    for (int c = 0; c < s->channels; c++)
      put_sample(&p, (c & 1) ? f.r : f.l, s->subslot);
  }
  return p - pkt;
}

static void status_text(char *out, size_t size, int solo_ok, int cap_ok) {
  double ring_ms = g_fill * 1000.0 / SOLO_RATE;
  double usb_ms = st.usb_queued * 1000.0 / PKTS_PER_SEC;
  snprintf(out, size,
           "solo: %s | cattura: %s\n"
           "latenza nostra: %.1f ms (ring %.1f + USB %.1f), min ring ultima finestra %zu frame\n"
           "pacchetti %ld, silenzio %ld, underrun %ld, drop %ld, dup %ld, USB in ritardo %ld\n"
           "salti %ld (%ld frame), sample rate Solo %u Hz\n"
           "FIFO USB %d pacchetti, riconnessioni solo %ld, riavvii cattura %ld, overrun cattura %ld\n",
           solo_ok ? "collegata" : "assente", cap_ok ? "attiva" : "in attesa",
           ring_ms + usb_ms, ring_ms, usb_ms,
           drift.win_min == (size_t)-1 ? 0 : drift.win_min,
           st.packets, st.silent, st.underruns, st.drops, st.dups, st.usb_late,
           st.jumps, st.jumped, st.solo_rate,
           st.fifo_depth, st.solo_reconnects, st.cap_restarts, capture_overruns());
}

// --- main ------------------------------------------------------------------

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  signal(SIGTERM, stop_signal);
  signal(SIGINT, stop_signal);
  umask(0077);
  mkdir(LOG_DIR, 0700);
  int lfd=open(LOG_DIR "/instance.lock",O_CREAT|O_RDWR,0600);
  if (lfd<0 || flock(lfd,LOCK_EX|LOCK_NB)<0) {
    if (lfd>=0) close(lfd);
    fprintf(stderr,"TTGK: already running or cannot lock instance\n");
    return 1;
  }
  g_log=fopen(LOG_PATH,"a");
  log_line("TTGK audio bridge started (pid %d), no network listener",getpid());
  if (control_start()<0) log_line("Control unavailable; using saved/default volume");

  solo_t solo;
  int solo_ok = 0, cap_ok = 0, cap_loaded = 0, playing = 0;
  double next_solo = 0, next_cap = 0, next_stats = now_sec() + STATS_EVERY;
  static float rec[CAPTURE_RECORD / sizeof(float)];
  uint8_t pkt[512];
  size_t pkt_len = 0;          // pacchetto pronto ma non ancora accettato
  double usb_t0 = 0;           // stima: pacchetti consumati = (t - usb_t0) * 8000
  long usb_written = 0;
  int volume_applied=-1, result=0;

  while (g_running && !control_stop_requested()) {
    double t = now_sec();
    int busy = 0;

    int wanted=control_volume();

    if (!solo_ok && t >= next_solo) {
      if (solo_open(&solo, 1, wanted) == 0) {
        solo_ok = 1;
        playing = 0;
        pkt_len = 0;
        reset_ring();
        volume_applied=wanted;
        control_applied(wanted);
        st.solo_reconnects++;
        st.solo_rate = solo.rate;
        // Misura la profondita' del FIFO USB riempiendolo di silenzio.
        size_t len = silent_packet(pkt, &solo);
        int depth = 0;
        while (depth < USB_AHEAD && solo_write(&solo, pkt, len) == 1)
          depth++;
        st.fifo_depth = depth;
        usb_written = depth;
        usb_t0 = now_sec();      // tutto il FIFO e' ancora da trasmettere
        log_line("Solo collegata: streaming a %d Hz, FIFO USB %d pacchetti (%.1f ms)",
                 SOLO_RATE, depth, depth * 1000.0 / PKTS_PER_SEC);
      } else {
        next_solo = t + 1.0;
      }
    }

    if (solo_ok && !cap_ok && t >= next_cap) {
      int r;
      if (!cap_loaded) {
        r = capture_open();
        if (r < 0) {
          log_line("cattura non disponibile: librerie non caricabili, esco");
          result=1;
          break;
        }
        cap_loaded = 1;
      } else {
        r = capture_restart();
        st.cap_restarts++;
      }
      if (r == 1) {
        cap_ok = 1;
        log_line("cattura audio di sistema attiva");
      } else {
        next_cap = t + 0.5;
      }
    }

    if (cap_ok && solo_ok) {
      int n=0;
      for (int blocks=0; blocks<8 && (n=capture_read(rec,sizeof(rec)))>0; blocks++) {
        ring_push(rec, n / (sizeof(float) * CAPTURE_CHANNELS));
        busy = 1;
      }
      if (n < 0) {
        log_line("sessione di cattura terminata, riprovo");
        cap_ok = 0;
        capture_close();
        reset_ring(); playing=0; pkt_len=0;
        next_cap = t + 0.5;
      }
    }

    if (solo_ok) {
      // L'USB consuma un pacchetto ogni 125us col clock della PS5: si scrive
      // solo quanto serve a restare USB_AHEAD pacchetti avanti, cosi' il FIFO
      // del kernel non aggiunge ritardo.
      for (int burst=0;burst<USB_AHEAD*2;burst++) {
        double queued = usb_written - (now_sec() - usb_t0) * PKTS_PER_SEC;
        if (queued < 0) {
          st.usb_late++;
          usb_t0 = now_sec() - (double)usb_written / PKTS_PER_SEC;
          queued = 0;
        }
        st.usb_queued = queued;
        if (queued >= USB_AHEAD)
          break;
        if (!pkt_len)
          pkt_len = build_packet(pkt, &solo, &playing);
        int w = solo_write(&solo, pkt, pkt_len);
        if (w == 1) {
          pkt_len = 0;
          usb_written++;
          st.packets++;
          busy = 1;
          continue;
        }
        if (w == 0) {
          // FIFO pieno: la stima era indietro, riallineala
          usb_t0 = now_sec() - (double)(usb_written - USB_AHEAD) / PKTS_PER_SEC;
        } else {
          log_line("Solo scollegata o errore USB (errno %d)", errno);
          solo_close(&solo);
          solo_ok = 0;
          capture_close(); cap_ok=0;
          reset_ring(); playing=0; pkt_len=0;
          next_solo = t + 1.0;
        }
        break;
      }
    }

    if (solo_ok && wanted!=volume_applied) {
      if (solo_set_volume(&solo,wanted)==0) {
        volume_applied=wanted; control_applied(wanted);
        log_line("Hardware volume: %.1f dB, mute=%d",(wanted&255)*0.5-74.0,!!(wanted&TTGK_MUTE));
      } else {
        log_line("Hardware volume failed (errno %d); stopping USB output",errno);
        solo_close(&solo); solo_ok=0; capture_close(); cap_ok=0;
        reset_ring(); playing=0; pkt_len=0; next_solo=t+1.0;
      }
    }
    if (t >= next_stats) {
      char s[512];
      status_text(s, sizeof(s), solo_ok, cap_ok);
      log_line("stato:\n%s", s);
      next_stats = t + STATS_EVERY;
    }

    if (!busy)
      usleep(250);
  }

  if (solo_ok)
    solo_close(&solo);
  if (cap_loaded)
    capture_close();
  control_stop();
  close(lfd);
  log_line("solobridge terminato");
  if (g_log)
    fclose(g_log);
  return result;
}
