#ifndef __JITTER_H__
#define __JITTER_H__

extern int __jitter_dbg__;

#ifndef JITTER_DBG
#define JITTER_DBG "none"
#endif

#ifdef DEBUG
#define jitter_dbg(jitter,format, ...) \
	do { \
		if (__jitter_dbg__ >= 0 && jitter->id == __jitter_dbg__) \
			fprintf(stderr, "\x1B[32mjitter_sg(%s) "format"\x1B[0m\n", jitter->name,  ##__VA_ARGS__); \
	} while (0)
#else
#define jitter_dbg(...)
#endif

typedef struct filter_audio_s filter_audio_t;
typedef struct filter_s filter_t;
typedef struct heartbeat_s heartbeat_t;

typedef enum jitte_s {
	JITTE_LOW,
	JITTE_MID,
	JITTE_HIGH,
} jitte_t;

typedef int (*consume_t)(void *consumer, unsigned char *buffer, size_t size);
typedef int (*produce_t)(void *producter, unsigned char *buffer, size_t size);
typedef struct jitter_ctx_s jitter_ctx_t;
struct jitter_ctx_s
{
	int id;
	const char *name;
	unsigned int count;
	size_t size;
	unsigned int thredhold;
	consume_t consume;
	void *consumer;
	produce_t produce;
	void *producter;
	unsigned int frequence;
	heartbeat_t *heartbeat;
	void *private;
};

typedef struct jitter_ops_s jitter_ops_t;
struct jitter_ops_s
{
	heartbeat_t *(*heartbeat)(jitter_ctx_t *, heartbeat_t *new);
	void (*lock)(jitter_ctx_t *);
	void (*reset)(jitter_ctx_t *);
	unsigned char *(*pull)(jitter_ctx_t *);
	void (*push)(jitter_ctx_t *, size_t len, void *beat);
	unsigned char *(*peer)(jitter_ctx_t *, void **beat);
	void (*pop)(jitter_ctx_t *, size_t len);
	void (*flush)(jitter_ctx_t *);
	size_t (*length)(jitter_ctx_t*);
	int (*empty)(jitter_ctx_t *);
	void (*pause)(jitter_ctx_t *jitter, int enable);
};

#define JITTER_AUDIO 0x80000000
#define JITTER_VIDEO 0xC0000000
#define JITTER_OTHER 0xE0000000
typedef enum jitter_format_e
{
	PCM_8bits_mono = JITTER_AUDIO,
	PCM_16bits_LE_mono,
	PCM_16bits_LE_stereo,
	PCM_24bits3_LE_stereo,
	PCM_24bits4_LE_stereo,
	PCM_32bits_LE_stereo,
	PCM_32bits_BE_stereo,
	MPEG2_3_MP3,
	FLAC,
	MPEG2_1 = JITTER_VIDEO,
	MPEG2_2,
	DVB_frame,
	SINK_BITSSTREAM = JITTER_OTHER,
} jitter_format_t;

typedef struct jitter_s jitter_t;
struct jitter_s
{
	jitter_ctx_t *ctx;
	const jitter_ops_t *ops;
	void (*destroy)(jitter_t *);
	jitter_format_t format;
};

#define JITTER_TYPE_SG 0x01
#define JITTER_TYPE_RING 0x02
jitter_t *jitter_init(int type, const char *name, unsigned count, size_t size);
void jitter_destroy(jitter_t *jitter);
inline int jitter_samplerate(jitter_t *jitter) {return jitter->ctx->frequence;};

#endif
