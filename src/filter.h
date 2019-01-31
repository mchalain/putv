#ifndef __FILTER_H__
#define __FILTER_H__

#include "jitter.h"

# define SIZEOF_INT 4

#if SIZEOF_INT >= 4
typedef   signed int sample_t;
#else
typedef   signed long sample_t;
#endif

#define MAXCHANNELS 8
typedef struct filter_audio_s filter_audio_t;
struct filter_audio_s
{
	sample_t *samples[MAXCHANNELS];
	int nsamples;
	int samplerate;
	char bitspersample;
	char nchannels;
	char regain;
};

#ifndef FILTER_CTX
typedef void filter_ctx_t;
#endif
typedef struct filter_ops_s filter_ops_t;
struct filter_ops_s
{
	filter_ctx_t *(*init)(unsigned int rate, jitter_format_t format);
	int (*run)(filter_ctx_t *ctx, filter_audio_t *audio, unsigned char *buffer, size_t size);
	void (*destroy)(filter_ctx_t *);
};

typedef struct filter_s filter_t;
struct filter_s
{
	const filter_ops_t *ops;
	filter_ctx_t *ctx;
};

extern const filter_ops_t *filter_pcm;
extern const filter_ops_t *filter_pcm_scaling;
#endif
