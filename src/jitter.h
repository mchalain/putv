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
			fprintf(stderr, "\x1B[32mjitter(%s) "format"\x1B[0m\n", jitter->name,  ##__VA_ARGS__); \
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
	heartbeat_t *(*heartbeat)(jitter_ctx_t *, heartbeat_t *);
	void (*lock)(jitter_ctx_t *);
	void (*reset)(jitter_ctx_t *);
	unsigned char *(*pull)(jitter_ctx_t *);
	unsigned char *(*pull_channel)(jitter_ctx_t *,int);
	void (*push)(jitter_ctx_t *, size_t, void *);
	unsigned char *(*peer)(jitter_ctx_t *, void **);
	unsigned char *(*peer_channel)(jitter_ctx_t *, int, void **);
	void (*pop)(jitter_ctx_t *, size_t len);
	void (*flush)(jitter_ctx_t *);
	size_t (*length)(jitter_ctx_t*);
	int (*empty)(jitter_ctx_t *);
	void (*pause)(jitter_ctx_t *, int);
	unsigned int (*nbchannels)(jitter_ctx_t *);
};

#define JITTER_AUDIO		0x80000000L
#define JITTER_VIDEO		0xC0000000L
#define JITTER_OTHER		0xE0000000L
#define JITTER_AUDIO_INTERLEAVED	0x00010000L
#define JITTER_AUDIO_COMPRESSED		0x00020000L
#define JITTER_VIDEO_RGB	0x00010000L
#define JITTER_VIDEO_COMPRESSED		0x00020000L
#define JITTER_CHANNEL_1	0x00000001L
#define JITTER_CHANNEL_2	0x00000002L
#define JITTER_CHANNEL_MASK	0x0000000fL
#define JITTER_INT_LE		0x00008000L
#define JITTER_INT_8		0x00000100L
#define JITTER_INT_16		0x00000200L
#define JITTER_INT_24		0x00000300L
#define JITTER_INT_32		0x00000400L
#define JITTER_INT_MASK		0x00000700L
#define JITTER_INT_EXTRA	0x00000800L
#define JITTER_8BITS		JITTER_AUDIO | JITTER_INT_LE | JITTER_INT_8
#define JITTER_16BITS		JITTER_AUDIO | JITTER_INT_LE | JITTER_INT_16
#define JITTER_16BITS_INTERLEAVED	JITTER_AUDIO | JITTER_AUDIO_INTERLEAVED | JITTER_INT_LE | JITTER_INT_16
#define JITTER_24BITS_INTERLEAVED	JITTER_AUDIO | JITTER_AUDIO_INTERLEAVED | JITTER_INT_LE | JITTER_INT_24
#define JITTER_32BITS_INTERLEAVED	JITTER_AUDIO | JITTER_AUDIO_INTERLEAVED | JITTER_INT_LE | JITTER_INT_32
typedef enum jitter_format_e
{
	PCM_8bits_mono = JITTER_8BITS | JITTER_CHANNEL_1,
	PCM_16bits_LE_mono = JITTER_16BITS | JITTER_CHANNEL_1,
	PCM_16bits_LE_stereo = JITTER_16BITS_INTERLEAVED | JITTER_CHANNEL_2,
	PCM_24bits3_LE_stereo = JITTER_24BITS_INTERLEAVED | JITTER_CHANNEL_2,
	PCM_24bits4_LE_stereo = JITTER_24BITS_INTERLEAVED | JITTER_INT_EXTRA | JITTER_CHANNEL_2,
	PCM_32bits_LE_stereo = JITTER_32BITS_INTERLEAVED | JITTER_CHANNEL_2,
	PCM_32bits_BE_stereo = JITTER_AUDIO | JITTER_AUDIO_INTERLEAVED | JITTER_INT_32 | JITTER_CHANNEL_2,
	MPEG2_3_MP3 = JITTER_AUDIO | JITTER_AUDIO_COMPRESSED,
	FLAC,
	MPEG4_AAC,
	MPEG2_1 = JITTER_VIDEO | JITTER_VIDEO_COMPRESSED,
	MPEG2_2,
	DVB_frame,
	SINK_BITSSTREAM = JITTER_OTHER,
} jitter_format_t;

#define FORMAT_NCHANNELS(format)	(format & JITTER_CHANNEL_MASK)
#define FORMAT_SAMPLESIZE(format)	((((format & JITTER_INT_MASK) >> 8) + ((format & JITTER_INT_EXTRA) >> 11)) * 8)
#define FORMAT_SHIFT(format)		(((format & JITTER_INT_MASK) >> 8) * 8)
#define FORMAT_IS_AUDIO(format)		(format & JITTER_AUDIO)
#define FORMAT_IS_VIDEO(format)		(format & JITTER_VIDEO)
#define FORMAT_IS_COMPRESSED(format)	(format & JITTER_AUDIO_COMPRESSED)

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
