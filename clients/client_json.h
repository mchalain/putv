/*****************************************************************************
 * main.c
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
#ifndef __CLIENT_JSON_H__
#define __CLIENT_JSON_H__

typedef int (*client_event_prototype_t)(void *data, json_t *params);
typedef struct client_event_s client_event_t;
typedef struct client_data_s client_data_t;
struct client_data_s
{
	int sock;
	client_event_t *events;
	unsigned long int pid;
	client_event_prototype_t proto;
	void *data;
	enum
	{
		OPTION_ASYNC = 0x01,
	} options;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
};

int client_unix(const char *socketpath, client_data_t *data);
int client_eventlistener(client_data_t *data, const char *name, client_event_prototype_t proto, void *protodata);
int client_loop(client_data_t *data);

int client_next(client_data_t *data, client_event_prototype_t proto, void *protodata);
int client_play(client_data_t *data, client_event_prototype_t proto, void *protodata);
int client_pause(client_data_t *data, client_event_prototype_t proto, void *protodata);
int client_stop(client_data_t *data, client_event_prototype_t proto, void *protodata);

#endif
