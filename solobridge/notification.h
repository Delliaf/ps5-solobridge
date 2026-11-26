#ifndef TTGK_NOTIFICATION_H
#define TTGK_NOTIFICATION_H
#include <stdint.h>
#include <stddef.h>
typedef struct { int32_t type,req_id,priority,msg_id,target_id,user_id,unk1,unk2,app_id,error_num,unk3; uint8_t use_icon; char message[1024],uri[1024],extra[1024]; } notify_request;
_Static_assert(sizeof(notify_request)==3120,"notification ABI");
_Static_assert(offsetof(notify_request,message)==45,"notification message ABI");
int sceKernelSendNotificationRequest(int,notify_request *,size_t,int);
#endif
