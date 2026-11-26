/* GPL-3.0. Local diagnostic HTTP server; never runs on audio thread. */
#include "web.h"
#include "control.h"
#include "log.h"
#include "web_page.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#define ROOT "/data/solobridge"
#define PORT 9160
#define KEYFILE ROOT "/web_key"
#define DISABLE ROOT "/disable_web"
#define REQMAX 8192
static atomic_int active,quit;
static pthread_t worker;
static int started,listener=-1;
static char key[17];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static web_audio audio;
static uint64_t ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int nonblock(int fd) { int flags=fcntl(fd,F_GETFL,0); return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK); }
static int private_addr(uint32_t a) {
  a=ntohl(a);
  return (a>>24)==10 || (a>>24)==127 || (a>>20)==0xac1 || (a>>16)==0xc0a8 || (a>>16)==0xa9fe;
}
static int wait_fd(int fd,short events,uint64_t end) {
  while (!atomic_load(&quit)) {
    uint64_t t=ms(); if (t>=end) return -1;
    struct pollfd p={fd,events,0};
    int r=poll(&p,1,(int)(end-t));
    if (r>0) return (p.revents&events)?0:-1;
    if (r==0 || errno!=EINTR) return -1;
  }
  return -1;
}
static int send_all(int fd,const void *data,size_t n,uint64_t end) {
  const char *p=data;
  while(n) {
    if(wait_fd(fd,POLLOUT,end)<0) return -1;
    ssize_t w=send(fd,p,n,0);
    if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) continue;
    if(w<=0) return -1;
    p+=w;n-=(size_t)w;
  }
  return 0;
}
static void response(int fd,int code,const char *type,const void *body,size_t n) {
  const char *reason=code==200?"OK":code==400?"Bad Request":code==401?"Unauthorized":code==404?"Not Found":code==409?"Conflict":"Service Unavailable";
  char h[768];
  int k=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n",code,reason,type,n);
  uint64_t end=ms()+1500;
  if(k>0&&(size_t)k<sizeof(h)&&send_all(fd,h,(size_t)k,end)==0) send_all(fd,body,n,end);
}
static void plain(int fd,int code,const char *s) { response(fd,code,"text/plain; charset=utf-8",s,strlen(s)); }
static int authorized(const char *headers) {
  const char *p=headers; int count=0,ok=0;
  while (*p) {
    const char *e=strstr(p,"\r\n"); if(!e||e==p) break;
    static const char name[]="X-TTGK-Key:";
    if((size_t)(e-p)>=sizeof(name)-1 && !strncasecmp(p,name,sizeof(name)-1)) {
      p+=sizeof(name)-1; while(p<e&&(*p==' '||*p=='\t'))p++;
      const char *last=e;while(last>p&&(last[-1]==' '||last[-1]=='\t'))last--;
      unsigned diff=0; if(last-p==16) {for(int i=0;i<16;i++)diff|=(unsigned char)p[i]^(unsigned char)key[i];ok=!diff;} else ok=0;
      count++;
    }
    p=e+2;
  }
  return count==1&&ok;
}
static void status(int fd) {
  web_audio a; pthread_mutex_lock(&lock);a=audio;pthread_mutex_unlock(&lock);
  pad_snapshot p;control_pad_snapshot(&p);
  char b[2048];
  int n=snprintf(b,sizeof(b),"{\"usb\":%d,\"capture\":%d,\"playing\":%d,\"gate\":%d,\"hw\":%d,\"desired\":%d,\"verified\":%d,\"limit\":%d,\"online\":%d,\"ring\":%d,\"phase_ppm\":%d,\"queue\":%.2f,\"underruns\":%llu,\"late\":%llu,\"blocked\":%llu,\"reconnects\":%llu,\"source_drop\":%llu,\"pad\":{\"stage\":%d,\"error\":%u,\"handle\":%d,\"user\":%d,\"connected\":%d,\"phase\":%d,\"buttons\":%u,\"last_pressed\":%u,\"changes\":%u,\"reads\":%u,\"age_ms\":%llu}}",
    a.usb,a.capture,a.playing,a.gate,a.hw,control_volume(),control_verified(),control_limit(),control_online(),a.ring,a.phase,a.queue,
    (unsigned long long)a.underruns,(unsigned long long)a.late,(unsigned long long)a.blocked,(unsigned long long)a.reconnects,(unsigned long long)a.source_drop,p.stage,(unsigned)p.err,p.handle,p.user,p.connected,p.phase,p.buttons,p.last_pressed,p.changes,p.reads,(unsigned long long)(p.last_ms?ms()-p.last_ms:0));
  if(n<0||(size_t)n>=sizeof(b))plain(fd,503,"status unavailable");else response(fd,200,"application/json",b,(size_t)n);
}
static void logs(int fd,const char *path,size_t max) {
  FILE *fp=fopen(path,"rb");if(!fp){plain(fd,404,"log unavailable");return;}
  if(fseek(fp,0,SEEK_END)){fclose(fp);plain(fd,503,"log seek failed");return;}
  long end=ftell(fp); if(end<0||fseek(fp,end>(long)max?end-(long)max:0,SEEK_SET)){fclose(fp);plain(fd,503,"log seek failed");return;}
  size_t n=end>(long)max?max:(size_t)end;
  char *b=malloc(n?n:1);if(!b){fclose(fp);plain(fd,503,"memory");return;}
  n=fread(b,1,n,fp);fclose(fp);response(fd,200,"text/plain; charset=utf-8",b,n);free(b);
}
static int parse_num(const char *p,int *out) {
  if(!*p)return 0;
  unsigned v=0;
  for(;*p;p++){if(*p<'0'||*p>'9'||v>511)return 0;v=v*10+(unsigned)(*p-'0');}
  if(v>511)return 0;*out=(int)v;return 1;
}
static int persist_disable(void) {
  int fd=open(DISABLE,O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd<0)return -1;
  return close(fd);
}
static void serve(int fd) {
  char b[REQMAX+1]; size_t used=0; uint64_t end=ms()+1200;
  while(used<REQMAX) {
    if(wait_fd(fd,POLLIN,end)<0)return;
    ssize_t n=recv(fd,b+used,REQMAX-used,0);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
    if(n<=0)return;
    if(memchr(b+used,0,(size_t)n)){plain(fd,400,"invalid request");return;}
    used+=(size_t)n;b[used]=0;if(strstr(b,"\r\n\r\n"))break;
  }
  if(!strstr(b,"\r\n\r\n")){plain(fd,400,"headers too large");return;}
  char *line=strstr(b,"\r\n");if(!line)return;*line=0;
  char method[8],path[128],version[16],extra;
  if(sscanf(b,"%7s %127s %15s %c",method,path,version,&extra)!=3 ||
    (strcmp(version,"HTTP/1.1")&&strcmp(version,"HTTP/1.0"))){plain(fd,400,"invalid request");return;}
  if(!strcmp(method,"GET")&&!strcmp(path,"/")){response(fd,200,"text/html; charset=utf-8",web_page,sizeof(web_page)-1);return;}
  if(!authorized(line+2)){plain(fd,401,"access key required");return;}
  if(!strcmp(method,"GET")){
    if(!strcmp(path,"/api/status"))status(fd);
    else if(!strcmp(path,"/api/log"))logs(fd,ROOT "/log.txt",65536);
    else if(!strcmp(path,"/api/log/full"))logs(fd,ROOT "/log.txt",524288);
    else if(!strcmp(path,"/api/log/previous"))logs(fd,ROOT "/log.txt.1",524288);
    else plain(fd,404,"not found");
    return;
  }
  if(strcmp(method,"POST")){plain(fd,400,"method not supported");return;}
  int v;
  if(!strncmp(path,"/api/volume/",12)&&parse_num(path+12,&v)){
    if(!control_set(v)){plain(fd,409,"DAC offline or invalid value");return;}plain(fd,200,"requested; wait for readback");
  } else if(!strcmp(path,"/api/trace/on")||!strcmp(path,"/api/trace/off")){
    control_pad_trace(!strcmp(path,"/api/trace/on"));plain(fd,200,"OK");
  } else if(!strcmp(path,"/api/web/off")||!strcmp(path,"/api/web/disable")){
    if(!strcmp(path,"/api/web/disable")&&persist_disable()<0){plain(fd,503,"cannot persist disable");return;}
    plain(fd,200,"web panel stopped; audio continues");control_pad_trace(0);atomic_store(&quit,1);
  } else if(!strcmp(path,"/api/stop")){
    plain(fd,200,"payload stopping");control_request_stop();atomic_store(&quit,1);
  } else plain(fd,404,"not found");
}
static void *run(void *arg) {
  (void)arg;
  while(!atomic_load(&quit)&&!control_stop_requested()) {
    struct pollfd p={listener,POLLIN,0};int r=poll(&p,1,250);
    if(r<=0){if(r<0&&errno!=EINTR)break;continue;}
    if(!(p.revents&POLLIN))break;
    struct sockaddr_in peer; socklen_t len=sizeof(peer);
    int fd=accept(listener,(struct sockaddr*)&peer,&len);
    if(fd<0)continue;
    if(peer.sin_family==AF_INET&&private_addr(peer.sin_addr.s_addr)&&nonblock(fd)==0)serve(fd);
    close(fd);
  }
  close(listener);listener=-1;control_pad_trace(0);atomic_store(&active,0);
  log_line("Web panel stopped; audio lifecycle independent");
  return NULL;
}
static int make_key(void) {
  /* Fresh per launch; never expose via unauthenticated HTTP or log. */
  unsigned char bytes[8];int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
  size_t n=0;while(n<sizeof(bytes)){ssize_t r=read(fd,bytes+n,sizeof(bytes)-n);if(r<0&&errno==EINTR)continue;if(r<=0){close(fd);return -1;}n+=(size_t)r;}close(fd);
  for(unsigned i=0;i<sizeof(bytes);i++)snprintf(key+2*i,3,"%02x",bytes[i]);
  FILE *fp=fopen(KEYFILE ".tmp","w");if(!fp)return -1;
  int ok=fprintf(fp,"%s\n",key)>0;if(fclose(fp))ok=0;
  if(!ok||rename(KEYFILE ".tmp",KEYFILE)<0){unlink(KEYFILE ".tmp");return -1;}
  return 0;
}
int web_start(void) {
  if(access(DISABLE,F_OK)==0)return 0;
  if(make_key()<0)return -1;
  listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0)return -1;
  int yes=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(PORT);a.sin_addr.s_addr=htonl(INADDR_ANY);
  if(nonblock(listener)<0||bind(listener,(struct sockaddr*)&a,sizeof(a))<0||listen(listener,4)<0){close(listener);listener=-1;return -1;}
  atomic_store(&quit,0);atomic_store(&active,1);
  if(pthread_create(&worker,NULL,run,NULL)){atomic_store(&active,0);close(listener);listener=-1;return -1;}
  started=1;
  char msg[128];snprintf(msg,sizeof(msg),"TTGK Web :%d | Key: %s",PORT,key);
  control_notify(msg);log_line("Web panel started on port %d (private IPv4, key required)",PORT);
  return 1;
}
int web_active(void){return atomic_load(&active);}
void web_publish(const web_audio *s){
  if(pthread_mutex_trylock(&lock)==0){audio=*s;pthread_mutex_unlock(&lock);}
}
void web_stop(void){
  if(started){atomic_store(&quit,1);pthread_join(worker,NULL);started=0;}
}
