#ifndef STUB_NEUTRINO_H
#define STUB_NEUTRINO_H
#include <stdint.h>
#include <sys/types.h>
typedef int64_t rcvid_t;
typedef struct { int chid; } name_attach_t;
struct _pulse { unsigned char code; int scoid; };
struct _msg_info { int dummy; };
#define _PULSE_CODE_DISCONNECT 3
#define _IO_MAX 255
#define _NTO_TCTL_RUNMASK 1
#ifndef EOK
#define EOK 0
#endif
extern int MsgSend(int, const void *, size_t, void *, int);
extern int MsgReceive(int, void *, size_t, struct _msg_info *);
extern int MsgReply(rcvid_t, int, const void *, size_t);
extern int MsgError(rcvid_t, int);
extern int ConnectAttach(int, pid_t, int, int, int);
extern int ConnectDetach(int);
extern name_attach_t *name_attach(const char *, const char *, int);
extern int name_detach(name_attach_t *, int);
extern int ThreadCtl(int, ...);
extern int name_open(const char *, int);
extern int name_close(int);
extern int ChannelCreate(unsigned);
extern int ChannelDestroy(int);
#endif
