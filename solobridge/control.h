#ifndef CONTROL_H
#define CONTROL_H
#include <stdint.h>
int control_set(int v);
int control_test_notification(void);
int control_notification_result(void);
int control_adjust(int delta,int toggle);
int control_limit(void);
int control_verified(void);
int control_online(void);
void control_request_stop(void);
int control_start(void);
int control_volume(void);
void control_connected(void);
void control_disconnected(int failed);
void control_applied(int volume);
int control_stop_requested(void);
void control_stop(void);
#endif
