#ifndef __SINK_H__
#define __SINK_H__

typedef struct player_ctx_s player_ctx_t;
typedef struct jitter_s jitter_t;
typedef struct encoder_s encoder_t;
typedef struct encoder_ops_s encoder_ops_t;

#ifndef SINK_CTX
typedef void sink_ctx_t;
#endif
typedef struct sink_ops_s sink_ops_t;
struct sink_ops_s
{
	const char *name;
	const char *default_;
	sink_ctx_t *(*init)(player_ctx_t *, const char *soundcard);
	unsigned int (*attach)(sink_ctx_t *, encoder_t *encoder);
	jitter_t *(*jitter)(sink_ctx_t *, unsigned int index);
	int (*run)(sink_ctx_t *);
	void (*destroy)(sink_ctx_t *);
	const char *(*service)(sink_ctx_t *, int *port, const char **txt[]);
	const encoder_ops_t *(*encoder)(sink_ctx_t *);
	void (*eventlistener)(sink_ctx_t *ctx, event_listener_cb_t listener, void *arg);
};

typedef struct sink_s sink_t;
struct sink_s
{
	const sink_ops_t *ops;
	sink_ctx_t *ctx;
};

sink_t *sink_build(player_ctx_t *, const char *arg);

#endif
