#ifndef __PLAYER_H__
#define __PLAYER_H__

typedef enum
{
	STATE_UNKNOWN,
	STATE_STOP,
	STATE_PLAY,
	STATE_PAUSE,
	STATE_CHANGE,
	STATE_ERROR,
} state_t;

typedef enum
{
	EVENT_ONCHANGE,
} player_event_type_t;

typedef struct jitter_s jitter_t;
typedef struct src_s src_t;
typedef struct decoder_s decoder_t;
typedef struct encoder_s encoder_t;
typedef struct sink_s sink_t;
typedef struct media_s media_t;

typedef struct player_ctx_s player_ctx_t;
typedef void (*player_event_cb_t)(void *ctx, player_ctx_t *, state_t);

player_ctx_t *player_init();
int player_change(player_ctx_t *ctx, const char *mediapath, int random, int loop);
media_t *player_media(player_ctx_t *ctx);
int player_run(player_ctx_t *userdata, jitter_t *encoder_jitter);
void player_destroy(player_ctx_t *ctx);
int player_waiton(player_ctx_t *ctx, int state);
state_t player_state(player_ctx_t *ctx, state_t state);
void player_next(player_ctx_t *ctx);
void player_onchange(player_ctx_t *ctx, player_event_cb_t callback, void *cbctx);
int player_mediaid(player_ctx_t *ctx);

#endif
