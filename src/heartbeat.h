#ifndef __HEARTBEAT_H__
#define __HEARTBEAT_H__

#include <stdint.h>

#define MAXCHANNELS 8
typedef struct beat_samples_s beat_samples_t;
struct beat_samples_s
{
	uint16_t nsamples;
	int16_t nloops; // TOCHECK
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
	uint32_t length;
};

typedef struct heartbeat_bitrate_s heartbeat_bitrate_t;
struct heartbeat_bitrate_s
{
	unsigned int bitrate;
	unsigned int ms;
};

typedef struct beat_pulse_s beat_pulse_t;
struct beat_pulse_s
{
	uint32_t pulses;
};

typedef struct heartbeat_pulse_s heartbeat_pulse_t;
struct heartbeat_pulse_s
{
	unsigned int ms;
};

typedef union beat_s beat_t;
union beat_s
{
	unsigned long isset;
	struct beat_bitrate_s bitrate;
	struct beat_samples_s samples;
	struct beat_pulse_s pulse;
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
extern const heartbeat_ops_t *heartbeat_pulse;
#endif
#endif
