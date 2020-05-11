#ifndef __SRC_H__
#define __SRC_H__

#include "event.h"

#define MAX_ESTREAM 4

typedef struct player_ctx_s player_ctx_t;
typedef struct decoder_s decoder_t;
typedef struct jitter_s jitter_t;

#ifndef SRC_CTX
typedef void src_ctx_t;
#endif

typedef struct src_ops_s src_ops_t;
struct src_ops_s
{
	const char *name;
	const char *protocol;
	src_ctx_t *(*init)(player_ctx_t *, const char *path, const char *mime);
	int (*run)(src_ctx_t *);
	const char *(*mime)(src_ctx_t *ctx, int index);
	void (*eventlistener)(src_ctx_t *ctx, event_listener_cb_t listener, void *arg);
	int (*attach)(src_ctx_t *ctx, long index, decoder_t *decoder);
	decoder_t *(*estream)(src_ctx_t *ctx, long index);
	void (*destroy)(src_ctx_t *);
	/**
	 * for demux only
	 */
	jitter_t *(*jitter)(src_ctx_t *ctx, jitte_t jitte);
};

typedef struct src_s src_t;
struct src_s
{
	const src_ops_t *ops;
	src_ctx_t *ctx;
	int mediaid;
};

src_t *src_build(player_ctx_t *player, const char *url, const char *mime, int id);

extern const src_ops_t *src_file;
extern const src_ops_t *src_curl;
extern const src_ops_t *src_unix;
extern const src_ops_t *src_alsa;
extern const src_ops_t *src_udp;
#endif
