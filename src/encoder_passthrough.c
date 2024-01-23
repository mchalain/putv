/*****************************************************************************
 * encoder_passthrough.c
 * this file is part of https://github.com/ouistiti-project/putv
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include "player.h"
typedef struct encoder_ops_s encoder_ops_t;
typedef struct encoder_ctx_s encoder_ctx_t;
struct encoder_ctx_s
{
	const encoder_ops_t *ops;
	player_ctx_t *ctx;
	jitter_t *inout;
};
#define ENCODER_CTX
#include "encoder.h"
#include "jitter.h"
#include "media.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static encoder_ctx_t *encoder_init(player_ctx_t *ctx)
{
	encoder_ctx_t *encoder = calloc(1, sizeof(*encoder));
	encoder->ops = encoder_passthrough;

	return encoder;
}

static jitter_t *encoder_jitter(encoder_ctx_t *encoder)
{
	return encoder->inout;
}

static int encoder_run(encoder_ctx_t *encoder, jitter_t *jitter)
{
	encoder->inout = jitter;
	return 0;
}

static const char *encoder_mime(encoder_ctx_t *encoder)
{
	return mime_audiopcm;
}

static void encoder_destroy(encoder_ctx_t *encoder)
{
	free(encoder);
}

const encoder_ops_t *encoder_passthrough = &(encoder_ops_t)
{
	.name = "passthrough",
	.init = encoder_init,
	.type = ES_AUDIO,
	.jitter = encoder_jitter,
	.run = encoder_run,
	.mime = encoder_mime,
	.destroy = encoder_destroy,
};
