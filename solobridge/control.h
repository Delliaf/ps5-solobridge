#ifndef CONTROL_H
#define CONTROL_H
#include <stdint.h>
typedef struct { int stage,err,handle,user,connected,phase; uint32_t buttons,last_pressed,changes,reads; uint64_t last_ms; } pad_snapshot;
int control_set(int v);
int control_adjust(int delta,int toggle);
int control_limit(void);
int control_verified(void);
int control_online(void);
void control_request_stop(void);
void control_pad_snapshot(pad_snapshot *s);
void control_pad_trace(int on);
void control_notify(const char *msg);
int control_start(void);
void control_enable_pad(void);
int control_volume(void);
void control_connected(void);
void control_disconnected(int failed);
void control_applied(int volume);
int control_stop_requested(void);
void control_stop(void);
#endif
