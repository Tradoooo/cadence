/*
 * cadence_reply.h — request handling of the Cadence QNX service.
 */
#ifndef CADENCE_REPLY_H
#define CADENCE_REPLY_H

#include <stdint.h>
#include <sys/neutrino.h>

#include "cadence_msg.h"

/* Real-time configuration reported by CADENCE_REQ_STATUS. */
typedef struct {
    int      n_threads;
    int      ctx_size;
    int      max_tokens;
    int      priority;
    uint32_t runmask;
    char     model[128];
} cadence_status_t;

/* Answer a request with an error message. */
void cadence_reply_error(rcvid_t rcvid, int code, const char * message);

/* Answer a request with the real-time configuration of the service. */
void cadence_reply_status(rcvid_t rcvid, const cadence_status_t * status);

/* Answer a simple request with a PONG. */
void cadence_reply_pong(rcvid_t rcvid);

#endif /* CADENCE_REPLY_H */
