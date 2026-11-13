/*
 * cadence_reply.c — request handling of the Cadence QNX service.
 */
#include "cadence_reply.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void cadence_reply_error(rcvid_t rcvid, int code, const char * message)
{
    cadence_rsp_error rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_ERROR;
    rsp.code        = code;
    snprintf(rsp.message, sizeof(rsp.message), "%s", message);
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
}

void cadence_reply_status(rcvid_t rcvid, const cadence_status_t * status)
{
    cadence_rsp_status rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_STATUS;
    rsp.policy      = SCHED_FIFO;
    rsp.priority    = status->priority;
    rsp.runmask     = status->runmask;
    rsp.n_threads   = status->n_threads;
    rsp.ctx_size    = status->ctx_size;
    rsp.max_tokens  = status->max_tokens;
    snprintf(rsp.model, sizeof(rsp.model), "%.127s", status->model);
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
}

void cadence_reply_pong(rcvid_t rcvid)
{
    cadence_rsp_status rsp;

    memset(&rsp, 0, sizeof(rsp));
    rsp.hdr.magic   = CADENCE_MAGIC;
    rsp.hdr.version = CADENCE_PROTO_VERSION;
    rsp.hdr.type    = CADENCE_RSP_PONG;
    MsgReply(rcvid, EOK, &rsp, sizeof(rsp));
}
