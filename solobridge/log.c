/* GPL-3.0. Bounded best-effort log queue; audio never waits on disk. */
#include "log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define PATH "/data/solobridge/log.txt"
#define LIMIT (512*1024L)
#define COUNT 64u
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static atomic_int quit,started;
static atomic_uint dropped;
static unsigned head,fill;
typedef struct { time_t ts; char text[512]; } entry_t;
static entry_t queue[COUNT];
static FILE *open_log(void) {
  FILE *fp=fopen(PATH,"a");
  if (fp && fseek(fp,0,SEEK_END)==0 && ftell(fp)>=LIMIT) {
    fclose(fp); unlink(PATH ".1"); rename(PATH,PATH ".1"); fp=fopen(PATH,"a");
  }
  return fp;
}
static void *run(void *arg) {
  (void)arg;
  FILE *fp=open_log();
  for (;;) {
    entry_t msg; int have=0;
    pthread_mutex_lock(&lock);
    if (fill) { msg=queue[head]; head=(head+1)%COUNT; fill--; have=1; }
    pthread_mutex_unlock(&lock);
    if (!have) { if (atomic_load(&quit)) break; usleep(100000); continue; }
    if (!fp) fp=open_log();
    if (fp) {
      unsigned lost=atomic_exchange(&dropped,0);
      if (lost) fprintf(fp,"[logger] dropped %u messages\n",lost);
      struct tm tm; char ts[32]="";
      if (localtime_r(&msg.ts,&tm)) strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tm);
      fprintf(fp,"[%s] %s\n",ts,msg.text); fflush(fp);
      if (ftell(fp)>=LIMIT) { fclose(fp); fp=NULL; }
    }
  }
  if (fp) fclose(fp);
  return NULL;
}
int log_start(void) {
  if (pthread_create(&worker,NULL,run,NULL)) return -1;
  atomic_store(&started,1); return 0;
}
void log_line(const char *fmt,...) {
  if (!atomic_load(&started)) return;
  char msg[512]; va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap);
  if (pthread_mutex_trylock(&lock)) { atomic_fetch_add(&dropped,1); return; }
  if (fill==COUNT) atomic_fetch_add(&dropped,1);
  else {
    entry_t *e=&queue[(head+fill)%COUNT];
    e->ts=time(NULL); snprintf(e->text,sizeof(e->text),"%s",msg); fill++;
  }
  pthread_mutex_unlock(&lock);
}
void log_stop(void) {
  if (atomic_exchange(&started,0)) { atomic_store(&quit,1); pthread_join(worker,NULL); }
}
