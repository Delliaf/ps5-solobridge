#ifndef CONTROL_H
#define CONTROL_H
int control_start(void);
void control_enable_pad(void);
int control_volume(void);
void control_connected(void);
void control_disconnected(int failed);
void control_applied(int volume);
int control_stop_requested(void);
void control_stop(void);
#endif
