#ifndef __HEARTBEAT_H__
#define __HEARTBEAT_H__

#define MAXCHANNELS 8
typedef struct beat_samples_s beat_samples_t;
struct beat_samples_s
{
	unsigned int nsamples;
	int nloops;
};

typedef struct heartbeat_samples_s heartbeat_samples_t;
struct heartbeat_samples_s
{
	unsigned int samplerate;
	jitter_format_t format;
	unsigned int nchannels;
};

typedef struct beat_bitrate_s beat_bitrate_t;
struct beat_bitrate_s
{
	unsigned long length;
};

typedef struct heartbeat_bitrate_s heartbeat_bitrate_t;
struct heartbeat_bitrate_s
{
	unsigned int bitrate;
	unsigned int ms;
};

#ifndef HEARTBEAT_CTX
typedef void heartbeat_ctx_t;
#endif
typedef struct heartbeat_ops_s heartbeat_ops_t;
struct heartbeat_ops_s
{
	heartbeat_ctx_t *(*init)(void *arg);
	void (*start)(heartbeat_ctx_t *ctx);
	int (*wait)(heartbeat_ctx_t *ctx, void *data);
	int (*lock)(heartbeat_ctx_t *ctx);
	int (*unlock)(heartbeat_ctx_t *ctx);
	void (*destroy)(heartbeat_ctx_t *);
};

typedef struct heartbeat_s heartbeat_t;
struct heartbeat_s
{
	const heartbeat_ops_t *ops;
	heartbeat_ctx_t *ctx;
};

#ifdef HEARTBEAT
extern const heartbeat_ops_t *heartbeat_samples;
extern const heartbeat_ops_t *heartbeat_bitrate;
#endif
#endif
