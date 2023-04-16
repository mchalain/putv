#ifndef __PLAYER_H__
#define __PLAYER_H__

#define STATE_PAUSE_MASK 0x8000

#include "event.h"
#include "filter.h"

typedef enum
{
	STATE_UNKNOWN,
	STATE_STOP,
	STATE_PLAY,
	STATE_CHANGE,
	STATE_ERROR,
	STATE_PAUSE = STATE_PLAY | STATE_PAUSE_MASK,
} state_t;

typedef enum
{
	EVENT_ONCHANGE,
} player_event_type_t;

typedef enum
{
	ES_AUDIO,
	ES_VIDEO,
	ES_DATA,
} estream_t;

typedef struct jitter_s jitter_t;
typedef struct src_s src_t;
typedef struct decoder_s decoder_t;
typedef struct sink_s sink_t;
typedef struct media_s media_t;
typedef struct filter_s filter_t;

typedef struct player_ctx_s player_ctx_t;

player_ctx_t *player_init();
int player_change(player_ctx_t *ctx, const char *mediapath, int random, int loop, int now);
media_t *player_media(player_ctx_t *ctx);
int player_subscribe(player_ctx_t *userdata, estream_t type, jitter_t *encoder_jitter);
int player_run(player_ctx_t *userdata);
void player_destroy(player_ctx_t *ctx);
int player_waiton(player_ctx_t *ctx, int state);
state_t player_state(player_ctx_t *ctx, state_t state);
int player_next(player_ctx_t *ctx, int change);
void player_removeevent(player_ctx_t *ctx, int id);
int player_eventlistener(player_ctx_t *ctx, event_listener_cb_t callback, void *cbctx, char *name);
int player_mediaid(player_ctx_t *ctx);
src_t *player_source(player_ctx_t *ctx);
int player_play(player_ctx_t *ctx, int id);

#endif
