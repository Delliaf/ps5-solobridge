/* GPL-3.0. FreeBSD 11 USB_FS ABI; only used for explicitly requested tests. */
#ifndef TTGK_USBFS_H
#define TTGK_USBFS_H
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#define FS_SLOTS 3
#define FS_MAX 64
#define FS_BYTES 36
typedef struct {void **ppBuffer; uint32_t *pLength; uint32_t nFrames,aFrames; uint16_t flags,timeout,isoc_time_complete; int status;} fs_endpoint;
typedef struct {fs_endpoint *pEndpoints; uint8_t ep_index_max;} fs_init;
typedef struct {uint32_t max_bufsize,max_frames; uint16_t max_packet_length;uint8_t dev_index,ep_index,ep_no;} fs_open;
typedef struct {uint8_t ep_index;} fs_index;
_Static_assert(sizeof(fs_endpoint)==40 && offsetof(fs_endpoint,status)==32,"USB_FS endpoint ABI");
_Static_assert(sizeof(fs_init)==16 && sizeof(fs_open)==16,"USB_FS init/open ABI");
#define FS_START _IOW('U',192,fs_index)
#define FS_STOP _IOW('U',193,fs_index)
#define FS_COMPLETE _IOR('U',194,fs_index)
#define FS_INIT _IOW('U',195,fs_init)
#define FS_UNINIT _IOW('U',196,fs_index)
#define FS_OPEN _IOWR('U',197,fs_open)
#define FS_CLOSE _IOW('U',198,fs_index)
typedef struct {
  fs_endpoint ep[FS_SLOTS];
  void *ptr[FS_SLOTS][FS_MAX];
  uint32_t len[FS_SLOTS][FS_MAX];
  uint8_t data[FS_SLOTS][FS_MAX][FS_BYTES];
  unsigned n,opened,busy;
  int initialized;
  uint64_t calls,submitted,completed;
} usbfs;
int fs_setup(usbfs *f,int fd,unsigned n,unsigned slots);
int fs_submit(usbfs *f,int fd,unsigned slot);
int fs_complete(usbfs *f,int fd);
int fs_drain(usbfs *f,int fd);
int fs_cleanup(usbfs *f,int fd);
#endif
