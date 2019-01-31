#ifndef __SINK_H__
#define __SINK_H__

typedef struct player_ctx_s player_ctx_t;
typedef struct jitter_s jitter_t;

#ifndef SINK_CTX
typedef void sink_ctx_t;
#endif
typedef struct sink_s sink_t;
struct sink_s
{
	sink_ctx_t *(*init)(player_ctx_t *, const char *soundcard);
	jitter_t *(*jitter)(sink_ctx_t *decoder);
	int (*run)(sink_ctx_t *);
	void (*destroy)(sink_ctx_t *);
};

const sink_t *sink_get(sink_ctx_t *ctx);

extern const sink_t *sink_alsa;
extern const sink_t *sink_tinyalsa;
extern const sink_t *sink_file;
extern const sink_t *sink_unix;
#endif
