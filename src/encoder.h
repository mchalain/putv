#ifndef __ENCODER_H__
#define __ENCODER_H__

typedef struct player_ctx_s player_ctx_t;
typedef struct jitter_s jitter_t;
typedef struct sink_s sink_t;
typedef enum jitter_format_e jitter_format_t;

#ifndef ENCODER_CTX
typedef void encoder_ctx_t;
#endif
typedef struct encoder_ops_s encoder_ops_t;
struct encoder_ops_s
{
	encoder_ctx_t *(*init)(player_ctx_t *);
	jitter_t *(*jitter)(encoder_ctx_t *encoder);
	int (*run)(encoder_ctx_t *, jitter_t *);
	const char *(*mime)(encoder_ctx_t *);
	int (*samplerate)(encoder_ctx_t *);
	jitter_format_t (*format)(encoder_ctx_t *);
	void (*destroy)(encoder_ctx_t *);
};

typedef struct encoder_s encoder_t;
struct encoder_s
{
	const encoder_ops_t *ops;
	encoder_ctx_t *ctx;
};

const encoder_ops_t *encoder_check(const char *path);

extern const encoder_ops_t *encoder_passthrough;
extern const encoder_ops_t *encoder_lame;
extern const encoder_ops_t *encoder_flac;
#endif
