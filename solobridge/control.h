#ifndef CONTROL_H
#define CONTROL_H
int control_start(void);
int control_volume(void);
void control_applied(int volume);
int control_stop_requested(void);
void control_stop(void);
#endif
