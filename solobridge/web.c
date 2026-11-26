/* GPL-3.0. Local diagnostic HTTP server; never runs on audio thread. */
#include "web.h"
#include "control.h"
#include "log.h"
#include "web_page.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
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
#ifndef WEB_ROOT
#define WEB_ROOT "/data/solobridge"
#endif
#ifndef WEB_PORT
#define WEB_PORT 9160
#endif
#define ROOT WEB_ROOT
#define PORT WEB_PORT
#define WEB_STATUS ROOT "/web_status.txt"
#define DISABLE ROOT "/disable_web"
#define REQMAX 8192
static atomic_int active,quit;
static pthread_t worker;
static int started,listener=-1;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static web_audio audio={.hw=-1};
static uint64_t ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int nonblock(int fd) { return fcntl(fd,F_SETFL,O_NONBLOCK); }
static int private_addr(uint32_t a) {
  a=ntohl(a);
  return (a>>24)==10 || (a>>24)==127 || (a>>20)==0xac1 || (a>>16)==0xc0a8 || (a>>16)==0xa9fe;
}
static int wait_fd(int fd,int writing,uint64_t end) {
  if(fd<0||fd>=FD_SETSIZE) return -1;
  while (!atomic_load(&quit)) {
    uint64_t t=ms(); if(t>=end)return -1;
    unsigned left=(unsigned)(end-t); if(left>100)left=100;
    struct timeval tv={.tv_sec=0,.tv_usec=(long)left*1000};
    fd_set set; FD_ZERO(&set); FD_SET(fd,&set);
    int r=select(fd+1,writing?NULL:&set,writing?&set:NULL,NULL,&tv);
    if(r>0)return 0;
    if(r<0&&errno!=EINTR)return -1;
  }
  return -1;
}
static int send_all(int fd,const void *data,size_t n,uint64_t end) {
  const char *p=data;
  while(n) {
    if(wait_fd(fd,1,end)<0) return -1;
    ssize_t w=send(fd,p,n,0);
    if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) continue;
    if(w<=0) return -1;
    p+=w;n-=(size_t)w;
  }
  return 0;
}
static void response(int fd,int code,const char *type,const void *body,size_t n) {
  const char *reason=code==200?"OK":code==400?"Bad Request":code==403?"Forbidden":code==404?"Not Found":code==409?"Conflict":"Service Unavailable";
  char h[768];
  int k=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n",code,reason,type,n);
  uint64_t end=ms()+1500;
  if(k>0&&(size_t)k<sizeof(h)&&send_all(fd,h,(size_t)k,end)==0) send_all(fd,body,n,end);
}
static void plain(int fd,int code,const char *s) { response(fd,code,"text/plain; charset=utf-8",s,strlen(s)); }
/* No access key. Restrict browser requests to numeric LAN Host values;
 * require a non-secret custom header for mutations, and reject foreign Origin.
 * This is CSRF / DNS-rebinding mitigation, NOT authentication. */
static int header(const char *p,const char *name,char *out,size_t cap) {
  size_t nl=strlen(name); int found=0;
  out[0]=0;
  while(*p) {
    const char *e=strstr(p,"\r\n");if(!e||e==p)break;
    if((size_t)(e-p)>nl && p[nl]==':' && !strncasecmp(p,name,nl)) {
      if(found++)return -1;
      const char *v=p+nl+1;while(v<e&&(*v==' '||*v=='\t'))v++;
      const char *last=e;while(last>v&&(last[-1]==' '||last[-1]=='\t'))last--;
      size_t n=(size_t)(last-v);if(n>=cap)return -1;
      memcpy(out,v,n);out[n]=0;
    }
    p=e+2;
  }
  return found;
}
static int allowed_host(const char *host) {
  char ip[64];size_t n=strcspn(host,":");
  if(!n||n>=sizeof(ip))return 0;
  memcpy(ip,host,n);ip[n]=0;
  if(host[n]) {
    const char *p=host+n+1;
    if(!*p)return 0;
    for(;*p;p++)if(*p<'0'||*p>'9')return 0;
  }
  struct in_addr a;
  return inet_pton(AF_INET,ip,&a)==1&&private_addr(a.s_addr);
}
static int allowed_request(const char *headers,int post) {
  char host[96],origin[128],action[32],expected[128];
  if(header(headers,"Host",host,sizeof(host))!=1||!allowed_host(host))return 0;
  int n=header(headers,"Origin",origin,sizeof(origin));
  if(n<0)return 0;
  if(n) {snprintf(expected,sizeof(expected),"http://%s",host);if(strcmp(origin,expected))return 0;}
  if(post&&(header(headers,"X-TTGK-Action",action,sizeof(action))!=1||strcmp(action,"1")))return 0;
  return 1;
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
  if(v>511)return 0;
  *out=(int)v;return 1;
}
static int persist_disable(void) {
  int fd=open(DISABLE,O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd<0)return -1;
  return close(fd);
}
static void serve(int fd) {
  char b[REQMAX+1]; size_t used=0; uint64_t end=ms()+1200;
  while(used<REQMAX) {
    if(wait_fd(fd,0,end)<0)return;
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
  if(!allowed_request(line+2,!strcmp(method,"POST"))){plain(fd,403,"use numeric local IP; POST requires X-TTGK-Action: 1");return;}
  if(!strcmp(method,"GET")&&!strcmp(path,"/")){response(fd,200,"text/html; charset=utf-8",web_page,sizeof(web_page)-1);return;}

  if(!strcmp(method,"GET")){
    if(!strcmp(path,"/health"))plain(fd,200,"TTGK Web OK\n");
    else if(!strcmp(path,"/api/status"))status(fd);
    else if(!strcmp(path,"/api/log"))logs(fd,ROOT "/log.txt",65536);
    else if(!strcmp(path,"/api/log/full"))logs(fd,ROOT "/log.txt",524288);
    else if(!strcmp(path,"/api/log/previous"))logs(fd,ROOT "/log.txt.1",524288);
    else if(!strcmp(path,"/api/web/status"))logs(fd,WEB_STATUS,4096);
    else plain(fd,404,"not found");
    return;
  }
  if(strcmp(method,"POST")){plain(fd,400,"method not supported");return;}
  int v;
  if(!strncmp(path,"/api/volume/",12)&&parse_num(path+12,&v)){
    if(!control_set(v)){plain(fd,409,"DAC offline or invalid value");return;}plain(fd,200,"requested; wait for readback");
  } else if(!strcmp(path,"/api/volume/up")||!strcmp(path,"/api/volume/down")||!strcmp(path,"/api/volume/toggle")){
    int toggle=!strcmp(path,"/api/volume/toggle");
    if(!control_adjust(!strcmp(path,"/api/volume/up")?4:-4,toggle)){plain(fd,409,"DAC offline");return;}
    plain(fd,200,"requested; wait for readback");
  } else if(!strcmp(path,"/api/trace/on")||!strcmp(path,"/api/trace/off")){
    control_pad_trace(!strcmp(path,"/api/trace/on"));plain(fd,200,"OK");
  } else if(!strcmp(path,"/api/web/off")||!strcmp(path,"/api/web/disable")){
    if(!strcmp(path,"/api/web/disable")&&persist_disable()<0){plain(fd,503,"cannot persist disable");return;}
    plain(fd,200,"web panel stopped; audio continues");control_pad_trace(0);atomic_store(&quit,1);
  } else if(!strcmp(path,"/api/stop")){
    plain(fd,200,"payload stopping");control_request_stop();atomic_store(&quit,1);
  } else plain(fd,404,"not found");
}
static void startup_status(const char *stage,int port,int error) {
  char msg[256]; snprintf(msg,sizeof(msg),"Web %s port=%d errno=%d (%s)",stage,port,error,error?strerror(error):"OK");
  log_line("%s",msg); fprintf(stderr,"%s\n",msg); fflush(stderr);
  FILE *fp=fopen(WEB_STATUS ".tmp","w");
  if(fp){int ok=fprintf(fp,"%s\n",msg)>0;if(fclose(fp))ok=0;if(ok)rename(WEB_STATUS ".tmp",WEB_STATUS);else unlink(WEB_STATUS ".tmp");}
}
static int open_listener(int port) {
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0){startup_status("socket failed",port,errno);return -1;}
  int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  struct sockaddr_in a={0};
#if defined(__FreeBSD__) || defined(__PROSPERO__)
  a.sin_len=sizeof(a);
#endif
  a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_ANY);
  const char *step="bind failed";
  if(bind(fd,(struct sockaddr*)&a,sizeof(a))<0)goto fail;
  step="listen failed";if(listen(fd,4)<0)goto fail;
  step="nonblock failed";if(nonblock(fd)<0)goto fail;
  return fd;
fail: {
  int e=errno;close(fd);startup_status(step,port,e);errno=e;return -1;
  }
}
static void *run(void *arg) {
  (void)arg;
  int port=PORT;
  while(!atomic_load(&quit)&&!control_stop_requested()) {
    if(listener<0) {
      port=PORT;listener=open_listener(port);
      if(listener<0&&errno==EADDRINUSE){port=PORT+1;listener=open_listener(port);}
      if(listener<0){for(int i=0;i<20&&!atomic_load(&quit);i++)usleep(100000);continue;}
      startup_status("listening (LAN, no key)",port,0);atomic_store(&active,1);
    }
    /* Same BSD socket/accept path as upstream, no random device, no
     * notification API, no poll dependency for the listening socket. */
    struct sockaddr_in peer={0};socklen_t len=sizeof(peer);
    int fd=accept(listener,(struct sockaddr*)&peer,&len);
    if(fd<0) {
      int e=errno;
      if(e==EAGAIN||e==EWOULDBLOCK||e==EINTR||e==ECONNABORTED){usleep(50000);continue;}
      startup_status("accept failed; retry",port,e);close(listener);listener=-1;atomic_store(&active,0);
      usleep(250000);continue;
    }
    if(peer.sin_family==AF_INET&&private_addr(peer.sin_addr.s_addr)&&nonblock(fd)==0)serve(fd);
    close(fd);
  }
  if(listener>=0)close(listener);
  listener=-1;control_pad_trace(0);atomic_store(&active,0);
  startup_status("stopped",port,0);
  return NULL;
}
int web_start(void) {
  if(access(DISABLE,F_OK)==0){startup_status("disabled by file",PORT,0);return 0;}
  atomic_store(&quit,0);
  int err=pthread_create(&worker,NULL,run,NULL);
  if(err){startup_status("thread failed",PORT,err);return -1;}
  started=1;return 1;
}
int web_active(void){return atomic_load(&active);}
void web_publish(const web_audio *s){
  if(pthread_mutex_trylock(&lock)==0){audio=*s;pthread_mutex_unlock(&lock);}
}
void web_stop(void){
  if(started){atomic_store(&quit,1);pthread_join(worker,NULL);started=0;}
}
