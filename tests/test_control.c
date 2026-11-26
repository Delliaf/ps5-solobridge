/* Test the actual control worker and notification ABI against a mock API. */
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include "../solobridge/control.h"
#include "../solobridge/notification.h"
#include "../solobridge/version.h"
static atomic_int calls;
void log_line(const char *fmt,...){(void)fmt;}
int sceKernelSendNotificationRequest(int dev,notify_request *r,size_t n,int blocking){
    assert(dev==0&&blocking==0&&n==3120);
    assert(strstr(r->message,TTGK_VERSION));
    int call=atomic_fetch_add(&calls,1);
    if(call==0){assert(r->target_id==-1&&r->use_icon==1&&strstr(r->uri,"cxml://"));return -1;}
    if(call==1){assert(r->target_id==0&&r->use_icon==0&&r->uri[0]==0);return 0;}
    assert(r->target_id==-1&&r->use_icon==1);return 0;
}
static void wait_result(int count){
    for(int i=0;i<200;i++){
        if(atomic_load(&calls)>=count&&control_notification_result()==0)return;
        usleep(10000);
    }
    assert(!"notification timeout");
}
int main(void){
    assert(control_start()==0);wait_result(2);
    assert(control_test_notification()==1);wait_result(3);
    assert(control_set(108)==0);
    control_connected();assert(control_set(108)==1&&control_volume()==108);
    assert(control_adjust(4,0)==1&&control_volume()==112);
    control_applied(112);
    control_stop();
    FILE *fp=fopen(CONTROL_ROOT "/volume","r");assert(fp);
    int v=0;assert(fscanf(fp,"%d",&v)==1&&v==112);fclose(fp);
    assert(atomic_load(&calls)==3);
    puts("control persistence and version notification ABI/fallback tests passed");
}
