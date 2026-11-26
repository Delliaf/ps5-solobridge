#ifndef BRIDGE_LOG_H
#define BRIDGE_LOG_H
int log_start(void);
void log_line(const char *fmt,...);
void log_stop(void);
#endif
