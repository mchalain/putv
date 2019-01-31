#ifndef __JITTER_H__
#define __JITTER_H__

typedef struct filter_audio_s filter_audio_t;
typedef struct filter_s filter_t;

typedef int (*consume_t)(void *consumer, unsigned char *buffer, size_t size);
typedef int (*produce_t)(void *producter, unsigned char *buffer, size_t size);
typedef int (*heart_t)(void *heart, void *beat);
typedef struct jitter_ctx_s jitter_ctx_t;
struct jitter_ctx_s
{
	const char *name;
	unsigned int count;
	size_t size;
	unsigned int thredhold;
	consume_t consume;
	void *consumer;
	produce_t produce;
	void *producter;
	unsigned int frequence;
	heart_t heart;
	void *heart_ctx;
	void *private;
};

typedef struct jitter_ops_s jitter_ops_t;
struct jitter_ops_s
{
	void (*reset)(jitter_ctx_t *);
	unsigned char *(*pull)(jitter_ctx_t *);
	void (*push)(jitter_ctx_t *, size_t len, void *beat);
	unsigned char *(*peer)(jitter_ctx_t *);
	void (*pop)(jitter_ctx_t *, size_t len);
	void (*flush)(jitter_ctx_t *);
	size_t (*length)(jitter_ctx_t*);
	int (*empty)(jitter_ctx_t *);
	int (*wait)(jitter_ctx_t *);
};

typedef enum
{
	PCM_16bits_LE_mono,
	PCM_16bits_LE_stereo,
	PCM_24bits3_LE_stereo,
	PCM_24bits4_LE_stereo,
	PCM_32bits_LE_stereo,
	PCM_32bits_BE_stereo,
	MPEG2_3_MP3,
	FLAC,
	MPEG2_1,
	MPEG2_2,
	DVB_frame,
	SINK_BITSSTREAM,
} jitter_format_t;

typedef struct jitter_s jitter_t;
struct jitter_s
{
	jitter_format_t format;
	jitter_ctx_t *ctx;
	const jitter_ops_t *ops;
};

jitter_t *jitter_scattergather_init(const char *name, unsigned count, size_t size);
void jitter_scattergather_destroy(jitter_t *);
jitter_t *jitter_ringbuffer_init(const char *name, unsigned count, size_t size);
void jitter_ringbuffer_destroy(jitter_t *);
#endif
