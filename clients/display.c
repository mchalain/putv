/*****************************************************************************
 * display.c
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
#include <unistd.h>
#include <string.h>

#include <libgen.h>
#include <jansson.h>

#include <pthread.h>

#ifdef USE_INOTIFY
#include <sys/inotify.h>
#endif

#include "client_json.h"
#include "display.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

extern unsigned int crc32b(unsigned char *message);

unsigned int c_album;
unsigned int c_artist;
unsigned int c_title;
unsigned int c_genre;
unsigned int c_state;

typedef struct display_ctx_s display_ctx_t;
struct display_ctx_s
{
	int inotifyfd;
	int dirfd;
	display_t disp;
	const char *root;
	const char *name;
	char run;
};

static int display_info(display_ctx_t *ctx, json_t *info)
{
	display_t *disp = &ctx->disp;
	const unsigned char *key = NULL;
	json_t *value;
	json_object_foreach(info, key, value)
	{
		if (json_is_string(value))
		{
			const char *string = json_string_value(value);
			if (string != NULL)
			{
				disp->ops->print(disp->ctx, crc32b((unsigned char *)key), string);
			}
		}
	}

	return 0;
}

int display_default(void *eventdata, json_t *json_params)
{
	display_ctx_t *ctx = (display_ctx_t *)eventdata;
	display_t *disp = &ctx->disp;
	disp->ops->clear(disp->ctx);
	json_t *jid = json_object_get(json_params, "id");
	int id = -1;
	if (json_is_number(jid))
		id = json_integer_value(jid);

	json_t *jstate = json_object_get(json_params, "state");
	if (json_is_string(jstate))
	{
		const char *state = json_string_value(jstate);
		disp->ops->print(disp->ctx, c_state, state);
	}
	json_t *jinfo = json_object_get(json_params, "info");
	if (json_is_object(jinfo))
	{
		display_info(ctx, jinfo);
	}

	disp->ops->flush(disp->ctx);

	return 0;
}

int run_client(char * socketpath, display_ctx_t *display_data)
{
	client_event_prototype_t display = display_default;

	client_data_t data = {0};
	client_unix(socketpath, &data);

	client_eventlistener(&data, "onchange", display, display_data);
	int ret = client_loop(&data);
	return ret;
}

#ifdef USE_INOTIFY
#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))

static void *_check_socket(void *arg)
{
	display_ctx_t *ctx = (display_ctx_t *)arg;
	char *socketpath;
	socketpath = malloc(strlen(ctx->root) + 1 + strlen(ctx->name) + 1);
	sprintf(socketpath, "%s/%s", ctx->root, ctx->name);
	if (!access(socketpath, R_OK | W_OK))
	{
		run_client(socketpath, (void *)ctx);
	}
	while (ctx->run)
	{
		char buffer[BUF_LEN];
		int i = 0;
		int length = read(ctx->inotifyfd, buffer, BUF_LEN);

		if (length < 0)
		{
			err("read");
		}

		while (i < length)
		{
			struct inotify_event *event =
				(struct inotify_event *) &buffer[i];
			if (event->len)
			{
				if (event->mask & IN_CREATE)
				{
					sleep(1);
					if (!access(socketpath, R_OK | W_OK))
					{
						run_client(socketpath, ctx);
					}
				}
#if 0
				else if (event->mask & IN_DELETE)
				{
				}
				else if (event->mask & IN_MODIFY)
				{
					dbg("The file %s was modified.", event->name);
				}
#endif
			}
			i += EVENT_SIZE + event->len;
		}
	}
	free(socketpath);
}
#endif

static display_elem_t *catalog_generate(display_ops_t *display, void *arg)
{
	const generator_ops_t *generator = display->generator;
	const window_ops_t *window = &generator->window;
	display_elem_t *elems = NULL;
	display_elem_t *elem;

	int lineh = (int)(window->height(arg) * 0.20);
	int seph = (int)(window->height(arg) * 0.05);
	int sideright = (int)(window->width(arg) * 0.20);
	int sideleft = (int)(window->width(arg) * 0.70);
	int sidesep = (int)(window->width(arg) * 0.10);

	elem = generator->new_elem(arg, T_DIV, c_artist, 0, 0, sideleft, lineh);
	elem->setpadding(elem, window->getpadding(arg));
	elem->setfgcolor(elem, window->getfgcolor(arg));
	elem->setfont(elem, window->getlfont(arg));
	elem->settextalign(elem, ELEM_CENTER);
	elem->next = elems;
	elems = elem;

	elem = generator->new_elem(arg, T_DIV, c_title, 0, (lineh + seph) * 1, sideleft, lineh * 2 + seph);
	elem->setpadding(elem, window->getpadding(arg));
	elem->setfgcolor(elem, window->getfgcolor(arg));
	elem->setfont(elem, window->getlfont(arg));
	elem->settextalign(elem, ELEM_CENTER);
	elem->next = elems;
	elems = elem;

	elem = generator->new_elem(arg, T_DIV, c_album, 0, (lineh + seph) * 3, sideleft, lineh);
	elem->setpadding(elem, window->getpadding(arg));
	elem->setfgcolor(elem, window->getfgcolor(arg));
	elem->setfont(elem, window->getfont(arg));
	elem->settextalign(elem, ELEM_CENTER);
	elem->next = elems;
	elems = elem;

	elem = generator->new_elem(arg, T_DIV, c_state, sideleft + sidesep, (window->height(arg) / 2) - (lineh - seph ), sideright, lineh);
	elem->setpadding(elem, window->getpadding(arg));
	elem->setfgcolor(elem, window->getfgcolor(arg));
	elem->setfont(elem, window->getfont(arg));
	elem->appendfunc(elem, window->printborder, arg);
	elem->settextalign(elem, ELEM_CENTER);
	elem->next = elems;
	elems = elem;

	return elems;
}

void catalog_free(display_elem_t *elems)
{
	display_elem_t *elem = elems;
	while (elem != NULL)
	{
		elem = elems->next;
		elems->destroy(elems);
		elems = elem;
	}
}


#define DAEMONIZE 0x01
int main(int argc, char **argv)
{
	int mode = 0;
	display_ctx_t display_data = {
		.root = "/tmp",
		.name = basename(argv[0]),
#ifdef DISPLAY_DIRECTFB
		.disp.ops = display_directfb,
#else
		.disp.ops = display_console,
#endif
	};

	int opt;
	do
	{
		opt = getopt(argc, argv, "R:n:hDc");
		switch (opt)
		{
			case 'R':
				display_data.root = optarg;
			break;
			case 'n':
				display_data.name = optarg;
			break;
			case 'h':
				fprintf(stderr, "display -R <dir> -n <socketname> -D -c");
				fprintf(stderr, "display events from putv applications\n");
				fprintf(stderr, " -D         daemonize");
				fprintf(stderr, " -R <DIR>   change the socket directory directory");
				fprintf(stderr, " -n <NAME>  change the socket name");
				fprintf(stderr, " -c         force display on the console");
				return -1;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
			case 'c':
				display_data.disp.ops = display_console;
			break;
		}
	} while(opt != -1);

	if ((mode & DAEMONIZE) && fork() != 0)
	{
		return 0;
	}

	c_album = crc32b("album");
	c_title = crc32b("title");
	c_artist = crc32b("artist");
	c_genre = crc32b("genre");
	c_state = crc32b("state");

	display_data.disp.ctx = display_data.disp.ops->create(argc, argv);

	if (display_data.disp.ctx == NULL)
		return -1;
	display_elem_t *dom = NULL;
	if (display_data.disp.ops->setdom != NULL)
	{
		dom = catalog_generate(display_data.disp.ops, display_data.disp.ctx);
		display_data.disp.ops->setdom(display_data.disp.ctx, dom);
	}

#ifdef USE_INOTIFY
	display_data.inotifyfd = inotify_init();
	int dirfd = inotify_add_watch(display_data.inotifyfd, display_data.root,
					IN_MODIFY | IN_CREATE | IN_DELETE);
	display_data.run = 1;
	_check_socket((void *)&display_data);
#else
	char *socketpath = malloc(strlen(display_data.root) + 1 + strlen(display_data.name) + 1);
	sprintf(socketpath, "%s/%s", display_data.root, display_data.name);

	run_client(socketpath, &display_data);
	free(socketpath);
#endif
	if (dom != NULL)
		catalog_free(dom);
	display_data.disp.ops->destroy(display_data.disp.ctx);
	return 0;
}
