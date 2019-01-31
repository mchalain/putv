#ifndef __MEDIA_H__
#define __MEDIA_H__

#define RANDOM_DEVICE "/dev/hwrng"

extern const char const *mime_octetstream;

const char *utils_getmime(const char *path);
const char *utils_getpath(const char *url, const char *proto);

typedef struct media_ctx_s media_ctx_t;

typedef int (*media_parse_t)(void *arg, int id, const char *url, const char *info, const char *mime);

typedef enum
{
	MEDIA_LOOP,
	MEDIA_RANDOM,
} media_options_t;

typedef struct media_ops_s media_ops_t;
struct media_ops_s
{
	media_ctx_t *(*init)(const char *url, ...);
	void (*destroy)(media_ctx_t *ctx);

	int (*count)(media_ctx_t *ctx);
	int (*insert)(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
	int (*find)(media_ctx_t *ctx, int id,media_parse_t cb, void *data);
	int (*remove)(media_ctx_t *ctx, int id, const char *path);
	int (*list)(media_ctx_t *ctx, media_parse_t print, void *data);
	int (*play)(media_ctx_t *ctx, media_parse_t play, void *data);
	int (*next)(media_ctx_t *ctx);
	int (*end)(media_ctx_t *ctx);
	int (*options)(media_ctx_t *ctx, media_options_t option, int enable);
};

typedef struct media_s media_t;
struct media_s
{
	const media_ops_t *ops;
	media_ctx_t *ctx;
};

media_t *media_build(const char *url);

extern const media_ops_t *media_sqlite;
extern const media_ops_t *media_file;
extern const media_ops_t *media_dir;
#endif
