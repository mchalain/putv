#ifndef __MUX_H__
#define __MUX_H__

#include "event.h"

typedef struct player_ctx_s player_ctx_t;

#ifndef MUX_CTX
typedef void mux_ctx_t;
#endif
typedef struct mux_ops_s mux_ops_t;
struct mux_ops_s
{
	const char *protocol;
	mux_ctx_t *(*init)(player_ctx_t *player, const char *arg);
	jitter_t *(*jitter)(mux_ctx_t *ctx, unsigned int index);
	unsigned int (*attach)(mux_ctx_t *ctx, encoder_t *encoder);
	int (*run)(mux_ctx_t *ctx, jitter_t *sink_jitter);
	const char *(*mime)(mux_ctx_t *ctx, unsigned int index);
	void (*destroy)(mux_ctx_t *ctx);
};

typedef struct mux_s mux_t;
struct mux_s
{
	const mux_ops_t *ops;
	mux_ctx_t *ctx;
};

mux_t *mux_build(player_ctx_t *player, const char *protocol, const char *search);

extern const mux_ops_t *mux_rtp;
extern const mux_ops_t *mux_mpegts;
extern const mux_ops_t *mux_passthrough;
#endif
