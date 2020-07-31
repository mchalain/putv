#ifndef __DECODER_H__
#define __DECODER_H__

#include "jitter.h"

typedef struct player_ctx_s player_ctx_t;
typedef struct jitter_s jitter_t;
typedef struct filter_s filter_t;

#ifndef DECODER_CTX
typedef void decoder_ctx_t;
#endif
typedef struct decoder_ops_s decoder_ops_t;
struct decoder_ops_s
{
	const char *name;
	int (*check)(const char *path);
	decoder_ctx_t *(*init)(player_ctx_t *);
	jitter_t *(*jitter)(decoder_ctx_t *decoder, jitte_t jitte);
	int (*prepare)(decoder_ctx_t *);
	int (*run)(decoder_ctx_t *, jitter_t *);
	const char *(*mime)(decoder_ctx_t *ctx);
	uint32_t (*position)(decoder_ctx_t *ctx);
	uint32_t (*duration)(decoder_ctx_t *ctx);
	void (*destroy)(decoder_ctx_t *);
};

typedef struct decoder_s decoder_t;
struct decoder_s
{
	const decoder_ops_t *ops;
	decoder_ctx_t *ctx;
};

decoder_t *decoder_build(player_ctx_t *player, const char *mime);

extern const decoder_ops_t *decoder_mad;
extern const decoder_ops_t *decoder_flac;
extern const decoder_ops_t *decoder_passthrough;
#endif
