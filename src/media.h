#ifndef __MEDIA_H__
#define __MEDIA_H__

#define RANDOM_DEVICE "/dev/hwrng"

typedef struct player_ctx_s player_ctx_t;

extern const char const *mime_octetstream;
extern const char const *mime_audiomp3;
extern const char const *mime_audioflac;
extern const char const *mime_audiopcm;

const char *utils_getmime(const char *path);
const char *utils_getpath(const char *url, const char *proto);
char *utils_parseurl(const char *url,
								char **protocol,
								char **host,
								char **port,
								char **path,
								char **search);
const char *utils_mime2mime(const char *mime);

typedef struct media_ctx_s media_ctx_t;

typedef int (*media_parse_t)(void *arg, int id, const char *url, const char *info, const char *mime);

typedef struct media_ops_s media_ops_t;
struct media_ops_s
{
	/**
	 * mandatory
	 */
	media_ctx_t *(*init)(player_ctx_t *player, const char *url, ...);
	/**
	 * mandatory
	 */
	void (*destroy)(media_ctx_t *ctx);

	/**
	 * mandatory
	 */
	int (*count)(media_ctx_t *ctx);
	/**
	 * optional
	 */
	int (*insert)(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
	/**
	 * mandatory
	 */
	int (*find)(media_ctx_t *ctx, int id,media_parse_t cb, void *data);
	/**
	 * optional
	 */
	int (*remove)(media_ctx_t *ctx, int id, const char *path);
	/**
	 * optional
	 */
	int (*list)(media_ctx_t *ctx, media_parse_t print, void *data);
	/**
	 * mandatory
	 */
	int (*play)(media_ctx_t *ctx, media_parse_t play, void *data);
	/**
	 * optional
	 */
	int (*next)(media_ctx_t *ctx);
	/**
	 * mandatory
	 */
	int (*end)(media_ctx_t *ctx);
	/**
	 * optional
	 */
	void (*random)(media_ctx_t *ctx, int enable);
	/**
	 * optional
	 */
	void (*loop)(media_ctx_t *ctx, int enable);
};

typedef struct media_s media_t;
struct media_s
{
	const media_ops_t *ops;
	media_ctx_t *ctx;
};

media_t *media_build(player_ctx_t *player, const char *path);
const char *media_path();

extern const media_ops_t *media_sqlite;
extern const media_ops_t *media_file;
extern const media_ops_t *media_dir;
#endif
