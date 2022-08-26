#ifndef __MEDIA_H__
#define __MEDIA_H__

#define RANDOM_DEVICE "/dev/hwrng"

typedef struct player_ctx_s player_ctx_t;
#include "jitter.h"

extern const char const *mime_octetstream;
extern const char const *mime_audiomp3;
extern const char const *mime_audioflac;
extern const char const *mime_audioalac;
extern const char const *mime_audioaac;
extern const char const *mime_audiopcm;
extern const char const *mime_directory;

extern const char const *str_title;
extern const char const *str_artist;
extern const char const *str_album;
extern const char const *str_track;
extern const char const *str_year;
extern const char const *str_genre;
extern const char const *str_date;
extern const char const *str_regain;
extern const char const *str_comment;
extern const char const *str_cover;
extern const char const *str_likes;

void utils_srandom();
const char *utils_getmime(const char *path);
char *utils_getpath(const char *url, const char *proto, char **query, int strict);
char *utils_parseurl(const char *url,
								char **protocol,
								char **host,
								char **port,
								char **path,
								char **search);
const char *utils_mime2mime(const char *mime);
const char *utils_format2mime(jitter_format_t format);

typedef struct media_ctx_s media_ctx_t;

typedef int (*media_parse_t)(void *arg, int id, const char *url, const char *info, const char *mime);

typedef enum option_state_e
{
	OPTION_REQUEST = -1,
	OPTION_DISABLE,
	OPTION_ENABLE,
} option_state_t;

struct media_filter_s
{
	const char *keyword;
	const char *artist;
	const char *album;
	const char *title;
	const char *speed;
	const char *genre;
	int like;
};
typedef struct media_filter_s media_filter_t;

typedef struct media_ops_s media_ops_t;
struct media_ops_s
{
	/**
	 * mandatory
	 */
	const char *name;
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
	 * optional
	 */
	int (*append)(media_ctx_t *ctx, const char *path, const char *info, const char *mime);
	/**
	 * optional
	 */
	int (*modify)(media_ctx_t *ctx, int id, const char *info);
	/**
	 * mandatory
	 */
	int (*find)(media_ctx_t *ctx, int id, media_parse_t cb, void *data);
	/**
	 * optional
	 */
	int (*filter)(media_ctx_t *ctx, media_filter_t *filter);
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
	int (*play)(media_ctx_t *ctx, int id, media_parse_t play, void *data);
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
	option_state_t (*random)(media_ctx_t *ctx, option_state_t enable);
	/**
	 * optional
	 */
	option_state_t (*loop)(media_ctx_t *ctx, option_state_t enable);
};

typedef struct media_s media_t;
struct media_s
{
	const media_ops_t *ops;
	media_ctx_t *ctx;
};

media_t *media_build(player_ctx_t *player, const char *path);
const char *media_path();
const char *media_parseinfo(const char *info, const char *key);
unsigned int media_boost(const char *info);

typedef struct json_t json_t;
#ifdef USE_ID3TAG
int media_parseid3tag(const char *path, json_t *object);
#endif
#ifdef USE_OGGMETADDATA
int media_parseoggmetadata(const char *path, json_t *object);
#endif
char *media_fillinfo(const char *url, const char *mime);
int media_parse_info(json_t *jinfo, char **ptitle, char **partist,
		char **palbum, char **pgenre, char **pcover, char **pcomment,
		int *plikes);

extern const media_ops_t *media_sqlite;
extern const media_ops_t *media_file;
extern const media_ops_t *media_dir;
#endif
