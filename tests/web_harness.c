/* Actual HTTP server on localhost with simulated hardware. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include "../solobridge/web.h"
#include "../solobridge/control.h"
#include "../solobridge/bridge_logic.h"
static atomic_int v=68,stop;
void log_line(const char *fmt,...) {(void)fmt;}
int control_test_notification(void){return 1;}
int control_notification_result(void){return 0;}
int control_stop_requested(void){return atomic_load(&stop);}
void control_request_stop(void){atomic_store(&stop,1);}
int control_volume(void){return atomic_load(&v);}
int control_verified(void){return control_volume();}
int control_limit(void){return 136;}
int control_online(void){return 1;}
int control_set(int n){if(!volume_valid(n))return 0;atomic_store(&v,n);return 1;}
int control_adjust(int d,int toggle){return control_set(toggle?(control_volume()^TTGK_MUTE):volume_step(control_volume(),d));}
static volatile sig_atomic_t external_stop;
static void terminate(int sig){(void)sig;external_stop=1;}
int main(void){
  signal(SIGTERM,terminate);
  int r=web_start();if(r<0)return 1;if(r==0)return 0;
  web_audio a={.usb=1,.capture=1,.playing=1,.gate=1,.hw=68,.ring=192};
  web_publish(&a);
  for(int i=0;i<500&&!web_active();i++)usleep(10000);
  if(!web_active()){web_stop();return 3;}
  puts("ready");fflush(stdout);
  while(web_active()&&!control_stop_requested()&&!external_stop){if(web_snapshot_requested())web_publish(&a);usleep(10000);}
  web_stop();return control_stop_requested()?2:0;
}
