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
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <libgen.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
# include <sys/ioctl.h>

#include <jansson.h>
#include <pthread.h>

#include "jsonrpc.h"
#include "client_json.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define client_dbg(...)

struct client_event_s
{
	const char *event;
	client_event_prototype_t proto;
	void *data;
	client_event_t *next;
};

static int method_subscribe(json_t *json_params, json_t **result, void *userdata)
{
	*result = json_pack("{ss}", "event", "onchange");
	return 0;
}

static int answer_proto(client_data_t * data, json_t *json_params)
{
	client_event_prototype_t func;
	void *funcdata;
	if (data->run == 0)
		return -1;

	pthread_mutex_lock(&data->mutex);
	data->pid = 0;
	func = data->proto;
	funcdata = data->data;
	data->proto = NULL;
	data->data = NULL;
	int ret = 0;
	if (func)
		ret = func(funcdata, json_params);
	data->result = ret;
	pthread_mutex_unlock(&data->mutex);
	pthread_cond_broadcast(&data->cond);
	return ret;
}

static int answer_subscribe(json_t *json_params, json_t **result, void *userdata)
{
	int state = -1;
	json_unpack(json_params, "{sb}", "subscribed", &state);
	client_data_t *data = userdata;

	answer_proto(data, json_params);

	return !state;
}

static int method_nullparam(json_t *json_params, json_t **result, void *userdata)
{
	*result = json_null();
	return 0;
}

static int answer_stdparams(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;

	int ret = answer_proto(data, json_params);
#ifdef DEBUG
	char * dump = json_dumps(json_params, JSON_INDENT(2));
	dbg("answer params %s", dump);
	free(dump);
#endif
	return ret;
}

static int method_stdparams(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	*result = data->params;
#ifdef DEBUG
	char * dump = json_dumps(*result, JSON_INDENT(2));
	dbg("method params %s", dump);
	free(dump);
#endif
	return 0;
}

static int notification_onchange(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	client_event_t *event = data->events;
	while (event)
	{
		if (!strcmp(event->event, "onchange"))
			break;
		event = event->next;
	}
	int ret = 0;
	if (event)
	{
		ret = event->proto(event->data, json_params);
	}
	return ret;
}

static int notification_onclose(json_t *json_params, json_t **result, void *userdata)
{
	client_data_t *data = userdata;
	client_event_t *event = data->events;
	while (event)
	{
		if (!strcmp(event->event, "onclose"))
			break;
		event = event->next;
	}
	int ret = 0;
	if (event)
	{
		ret = event->proto(event->data, json_params);
	}
	return ret;
}

struct jsonrpc_method_entry_t table[] =
{
	{'r',"subscribe", method_subscribe, "o", 0, NULL},
	{'a',"subscribe", answer_subscribe, "o", 0, NULL},
	{'r',"next", method_nullparam, "", 0, NULL},
	{'a',"next", answer_stdparams, "o", 0, NULL},
	{'r',"play", method_nullparam, "", 0, NULL},
	{'a',"play", answer_stdparams, "o", 0, NULL},
	{'r',"pause", method_nullparam, "", 0, NULL},
	{'a',"pause", answer_stdparams, "o", 0, NULL},
	{'r',"stop", method_nullparam, "", 0, NULL},
	{'a',"stop", answer_stdparams, "o", 0, NULL},
	{'r',"status", method_nullparam, "", 0, NULL},
	{'a',"status", answer_stdparams, "o", 0, NULL},
	{'r',"setnext", method_stdparams, "o", 0, NULL},
	{'a',"setnext", answer_stdparams, "o", 0, NULL},
	{'r',"volume", method_stdparams, "o", 0, NULL},
	{'a',"volume", answer_stdparams, "o", 0, NULL},
	{'r',"change", method_stdparams, "o", 0, NULL},
	{'a',"change", answer_stdparams, "o", 0, NULL},
	{'r',"append", method_stdparams, "o", 0, NULL},
	{'a',"append", answer_stdparams, "o", 0, NULL},
	{'r',"setinfo", method_stdparams, "o", 0, NULL},
	{'a',"setinfo", answer_stdparams, "o", 0, NULL},
	{'r',"remove", method_stdparams, "o", 0, NULL},
	{'a',"remove", answer_stdparams, "o", 0, NULL},
	{'r',"list", method_stdparams, "o", 0, NULL},
	{'a',"list", answer_stdparams, "o", 0, NULL},
	{'r',"info", method_stdparams, "o", 0, NULL},
	{'a',"info", answer_stdparams, "o", 0, NULL},
	{'r',"getposition", method_nullparam, "", 0, NULL},
	{'a',"getposition", answer_stdparams, "o", 0, NULL},
	{'r',"options", method_stdparams, "o", 0, NULL},
	{'a',"options", answer_stdparams, "o", 0, NULL},
	{'n',"onchange", notification_onchange, "o", 0, NULL},
	{0, NULL},
};

int client_unix(const char *socketpath, client_data_t *data)
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock > 0)
	{
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(struct sockaddr_un));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socketpath, sizeof(addr.sun_path));
		int ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr.sun_path));
		if (ret != 0)
		{
			close(sock);
			sock = 0;
			err("client: connect error %s", strerror(errno));
			return -1;
		}
		data->sock = sock;
		pthread_cond_init(&data->cond, NULL);
		pthread_mutex_init(&data->mutex, NULL);
	}

	return sock;
}

void client_async(client_data_t *data,int async)
{
	if (async)
		data->options |= OPTION_ASYNC;
	else
		data->options &= ~OPTION_ASYNC;
}

unsigned long int client_cmd(client_data_t *data, const char * cmd)
{
	int ret;
	char *buffer = jsonrpc_request(cmd, strlen(cmd), table, (char*)data, &data->pid);
	if (buffer == NULL)
	{
		err("cmd %s unknown", cmd);
		return 0;
	}
	unsigned long int pid = data->pid;
	client_dbg("client: send %s", buffer);
	ret = send(data->sock, buffer, strlen(buffer) + 1, MSG_NOSIGNAL);
	if (ret < 0)
	{
		err("jsonrpc: send error %s", strerror(errno));
		return -1;
	}
	if (data->options & OPTION_ASYNC)
		return 0;
	return pid;
}

int client_wait(client_data_t *data, unsigned long int pid)
{
	if ((data->options & OPTION_ASYNC) == OPTION_ASYNC)
		return 0;
	while (pid > 0 && data->pid == pid)
	{
		pthread_cond_wait(&data->cond, &data->mutex);
	}
	return data->result;
}

static int _client_generic(client_data_t *data, client_event_prototype_t proto, void *protodata, const char *cmd)
{
	if (data->pid > 0)
	{
		warn("client: request %lu waiting", data->pid);
		return -2;
	}
	pthread_mutex_lock(&data->mutex);
	data->proto = proto;
	data->data = protodata;
	data->result = 0;
	long int pid = client_cmd(data, cmd);
	if (pid == -1)
	{
		pthread_mutex_unlock(&data->mutex);
		return -1;
	}
	int ret = client_wait(data, (unsigned long int)pid);
	pthread_mutex_unlock(&data->mutex);
	return ret;
}

int client_next(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "next");
}

int client_play(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "play");
}

int client_pause(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "pause");
}

int client_stop(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "stop");
}

int client_status(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "status");
}

int client_getposition(client_data_t *data, client_event_prototype_t proto, void *protodata)
{
	return _client_generic(data, proto, protodata, "getposition");
}

int client_volume(client_data_t *data, client_event_prototype_t proto, void *protodata, int step)
{
	if (step != 0)
	{
		data->params = json_object();
		json_object_set_new(data->params, "step", json_integer(step));
	}
	else
		data->params = NULL;
	return _client_generic(data, proto, protodata, "volume");
}

int client_setnext(client_data_t *data, client_event_prototype_t proto, void *protodata, int id)
{
	data->params = json_object();
	json_object_set_new(data->params, "id", json_integer(id));
	return _client_generic(data, proto, protodata, "setnext");
}

int client_eventlistener(client_data_t *data, const char *name, client_event_prototype_t proto, void *protodata)
{
	client_event_t *event = calloc(1, sizeof(*event));
	event->event = strdup(name);
	event->proto = proto;
	event->data = protodata;
	event->next = data->events;
	data->events = event;
	return 0;
}

int media_change(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	data->params = media;
	return _client_generic(data, proto, protodata, "change");
}

int media_insert(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	data->params = media;
	return _client_generic(data, proto, protodata, "append");
}

int media_setinfo(client_data_t *data, client_event_prototype_t proto, void *protodata, int id, json_t *info)
{
	data->params = json_object();
	json_object_set_new(data->params, "id", json_integer(id));
	json_object_set_new(data->params, "info", info);
	return _client_generic(data, proto, protodata, "setinfo");
}

int media_remove(client_data_t *data, client_event_prototype_t proto, void *protodata, json_t *media)
{
	data->params = media;
	return _client_generic(data, proto, protodata, "remove");
}

int media_list(client_data_t *data, client_event_prototype_t proto, void *protodata, int first, int maxitems)
{
	data->params = json_object();
	json_object_set_new(data->params, "first", json_integer(first));
	json_object_set_new(data->params, "maxitems", json_integer(maxitems));
	return _client_generic(data, proto, protodata, "list");
}

int media_info(client_data_t *data, client_event_prototype_t proto, void *protodata, int id)
{
	data->params = json_object();
	json_object_set_new(data->params, "id", json_integer(id));
	return _client_generic(data, proto, protodata, "info");
}

int media_options(client_data_t *data, client_event_prototype_t proto, void *protodata, int random, int loop)
{
	data->params = json_object();
	if (random == 1)
		json_object_set_new(data->params, "random", json_true());
	else if (random == 0)
		json_object_set_new(data->params, "random", json_false());
	if (loop == 1)
		json_object_set_new(data->params, "loop", json_true());
	else if (loop == 0)
		json_object_set_new(data->params, "loop", json_false());
	return _client_generic(data, proto, protodata, "options");
}

static int _media_wait(void *eventdata, json_t *json_params)
{
	pthread_cond_t *cond = (pthread_cond_t *)eventdata;
	pthread_cond_broadcast(cond);
	return 0;
}

int media_wait(client_data_t *data, int nb)
{
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	pthread_cond_init(&cond, NULL);
	pthread_mutex_init(&mutex, NULL);

	client_eventlistener(data, "onchange", _media_wait, &cond);
	while (nb > 0)
	{
		pthread_cond_wait(&cond, &mutex);
		nb --;
	}
	pthread_cond_destroy(&cond);
	pthread_mutex_destroy(&mutex);
	return 0;
}

#ifdef JSONRPC_LARGEPACKET
static size_t recv_cb(void *buffer, size_t len, void *arg)
{
	int ret;
	client_data_t *data = (client_data_t *)arg;
	if (data->message == NULL)
	{
		ret = recv(data->sock, buffer, len, MSG_NOSIGNAL);
		if (ret <= 0)
		{
			err("client: connection error %s", strerror(errno));
			strcpy(buffer, "{}");
			client_disconnect(data);
			return -1;
		}
	}
	else
	{
		ret = data->messagelen;
		memcpy(buffer, data->message, data->messagelen);
		free(data->message);
		data->message = NULL;
	}
	int length = strlen(buffer);
	if (ret >= 0)
	{
		if (length == ret && strstr(buffer, "}{") != NULL)
		{
			length = (strstr(buffer, "}{") - (char *)buffer) + 1;
		}
		if (length < ret)
		{
			err("client: two messages in ONE");
			client_dbg("client: recv %ld/%d" , strlen(buffer), ret);
			data->message = malloc(ret - length);
			memcpy(data->message, buffer + length, ret - length);
			data->messagelen = ret - length;
			ret = length;
		}
	}

	client_dbg("client: recv %d %s", ret, (char *)buffer);
	return ret;
}
#endif

json_t *client_error_response(json_t *json_id, json_t *json_error, void *arg)
{
	client_data_t *data = (client_data_t *)arg;
	data->pid = 0;
	return jsonrpc_request_error_response(json_id, json_error, arg);
}

int client_loop(client_data_t *data)
{
	int ret = -1;
	client_event_t *event = data->events;
	while (event)
	{
//		jsonrpc_request("subscribe");
		event = event->next;
	}
	data->run = 1;
	jsonrpc_set_errorhandler(client_error_response);
	while (data->sock > 0  && data->run)
	{
		fd_set rfds;
		int maxfd = data->sock;
		struct timeval timeout = {5,0};
		FD_ZERO(&rfds);
		FD_SET(data->sock, &rfds);
		ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);
		if (ret > 0 && FD_ISSET(data->sock, &rfds))
		{
#ifdef JSONRPC_LARGEPACKET
			do
			{
				json_error_t error;
				json_t *response = json_load_callback(recv_cb, (void *)data, JSON_DISABLE_EOF_CHECK, &error);
				if (response != NULL)
				{
					jsonrpc_jresponse(response, table, data);
					ret = 0;
				}
				else
				{
					err("client: receive mal formated json %s", error.text);
					data->pid = -1;
					data->run = 0;
					ret = -1;
				}
			} while (data->message != NULL);
#else
			int len;
			ret = ioctl(data->sock, FIONREAD, &len);
			char *buffer = malloc(len + 1);
			ret = recv(data->sock, buffer, len, MSG_NOSIGNAL);
			if (ret > 0)
			{
				buffer[ret] = 0;
				jsonrpc_handler(buffer, strlen(buffer), table, data);
				ret = 0;
			}
			else //if (ret == 0)
				data->run = 0;
#endif
		}
		else if (ret == 0) //timeout
		{
			data->pid = 0;
		}
	}
	notification_onclose(NULL, NULL, data);
	dbg("client: end loop");
	pthread_cond_destroy(&data->cond);
	pthread_mutex_destroy(&data->mutex);
	return ret;
}

void client_disconnect(client_data_t *data)
{
	data->run = 0;
	if (data->sock > 0)
	{
		shutdown(data->sock, SHUT_WR);
		close(data->sock);
		data->sock = 0;
	}
}
