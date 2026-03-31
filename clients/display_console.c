/*****************************************************************************
 * display_console.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

typedef struct display_console_ctx_s
{
	int linelength;
} display_console_ctx_t;

static void * display_create(int argc, char** argv)
{
	display_console_ctx_t *ctx;
	ctx = calloc(1, sizeof(ctx));
	ctx->linelength = 40;
	return ctx;
}

static int display_print(void *arg, int type, const char *string)
{
	display_console_ctx_t *data = (display_console_ctx_t *)arg;
	int length = strlen(string);
	int indent = (data->linelength - length) / 2;

	if (indent < 0)
	{
		char *cut = strstr(string," ");
		if (cut != NULL)
		{
			*cut = 0;
			cut++;
			display_print(data, type, string);
		}
		else
		{
			printf("%.*s\n", data->linelength, string);
		}
	}
	else
	{
		printf("%*s%s\n",indent,"",string);
	}
	return 0;
}

static void display_clear(void *arg)
{
}

static void display_flush(void *arg)
{
}

static void display_destroy(void *arg)
{
	free(arg);
}

display_ops_t *display_console = &(display_ops_t)
{
	.create = display_create,
	.clear = display_clear,
	.print = display_print,
	.flush = display_flush,
	.destroy = display_destroy,
};
