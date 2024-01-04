#ifndef __DEMUX_H__
#define __DEMUX_H__

#include "event.h"
#include "jitter.h"

typedef struct player_ctx_s player_ctx_t;

#ifndef DEMUX_CTX
typedef void demux_ctx_t;
#endif
typedef struct src_ops_s demux_ops_t;

typedef struct src_s demux_t;

demux_t *demux_build(player_ctx_t *player, const char *url, const char *mime);

extern const demux_ops_t *demux_rtp;
extern const demux_ops_t *demux_mpegts;
extern const demux_ops_t *demux_dvb;
extern const demux_ops_t *demux_passthrough;

#endif
