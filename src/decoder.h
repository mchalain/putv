#ifndef __DECODER_H__
#define __DECODER_H__

typedef struct player_ctx_s player_ctx_t;
typedef struct jitter_s jitter_t;

#ifndef DECODER_CTX
typedef void decoder_ctx_t;
#endif
typedef struct decoder_s decoder_t;
struct decoder_s
{
	int (*check)(char *path);
	decoder_ctx_t *(*init)(player_ctx_t *);
	jitter_t *(*jitter)(decoder_ctx_t *decoder);
	int (*run)(decoder_ctx_t *, jitter_t *);
	void (*destroy)(decoder_ctx_t *);
	const char *mime;
};

const decoder_t *decoder_get(decoder_ctx_t *);

extern const decoder_t *decoder_mad;
extern const decoder_t *decoder_flac;
extern const decoder_t *decoder_passthrough;
#endif
