/*****************************************************************************
 * src_udp.c
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
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pwd.h>

#include "player.h"
#include "jitter.h"
#include "event.h"
typedef struct src_ops_s src_ops_t;
typedef struct src_ctx_s src_ctx_t;
typedef struct src_s demux_t;
struct src_ctx_s
{
	player_ctx_t *player;
	int sock;
	state_t state;
	pthread_t thread;
	jitter_t *out;
	unsigned int samplerate;
	char *host;
	int port;
	struct sockaddr_in saddr;
	struct sockaddr_in6 saddr6;
	struct sockaddr *addr;
	int addrlen;
	const char *mime;
#ifdef DEMUX_PASSTHROUGH
	demux_t *demux;
#else
	decoder_t *estream;
	event_listener_t *listener;
#endif
#ifdef UDP_DUMP
	int dumpfd;
#endif
};
#define SRC_CTX
#include "src.h"
#include "demux.h"
#include "media.h"
#include "decoder.h"

#ifdef TINYSVCMDNS
#include "tinysvcmdns/mdns.h"
#endif

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define src_dbg(...)

#define SRC_POLICY REALTIME_SCHED
#define SRC_PRIORITY 65

/**
 * UDP_THREAD: This feature should minimize the resources, but in fact
 * to receiv data in the decoding thread is too slow and the thread is often
 * not ready to receive UDP packet.
 * The feature is alway possible to remember that is not a good solution.
 */
static const char *jitter_name = "udp socket";

static int _src_connect(src_ctx_t *ctx, const char *host, int iport)
{
	int count = 2;

	int ret;
	int af = AF_INET;

	in_addr_t inaddr;
	struct in6_addr in6addr;
	ret = inet_pton(af, host, &inaddr);
	if (ret != 1)
	{
		af = AF_INET6;
		ret = inet_pton(AF_INET6, host, &in6addr);
	}

	int sock = socket(af, SOCK_DGRAM, IPPROTO_UDP);
	//TODO: try IPPROTO_UDPLITE
	if (sock == 0)
	{
		err("src: udp socket error");
		return -1;
	}

	struct sockaddr *addr = NULL;
	int addrlen = 0;
	struct sockaddr_in saddr;
	struct sockaddr_in6 saddr6;
	if (af == AF_INET)
	{
		memset(&saddr, 0, sizeof(struct sockaddr_in));
		saddr.sin_family = PF_INET;
		saddr.sin_addr.s_addr = INADDR_ANY; // bind socket to any interface
		saddr.sin_port = htons(iport); // Use the first free port
		addr = (struct sockaddr*)&saddr;
		addrlen = sizeof(saddr);
	}
	else
	{
		memset(&saddr6, 0, sizeof(struct sockaddr_in6));
		saddr6.sin6_family = PF_INET6;
		memcpy(&saddr6.sin6_addr, &in6addr_any, sizeof(saddr6.sin6_addr)); // bind socket to any interface
		saddr6.sin6_port = htons(iport); // Use the first free port
		addr = (struct sockaddr*)&saddr6;
		addrlen = sizeof(saddr6);
	}

	ret = bind(sock, addr, addrlen);
	if (ret < 0)
	{
		err("src: bind error %s", strerror(errno));
		goto err;
	}
	int value=1;
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));

	if ((af == AF_INET) && IN_MULTICAST(htonl(inaddr)))
	{
		struct ip_mreq imreq;
		memset(&imreq, 0, sizeof(struct ip_mreq));
		imreq.imr_multiaddr.s_addr = inaddr;
		imreq.imr_interface.s_addr = htonl(INADDR_ANY); // use DEFAULT interface

		// JOIN multicast group on default interface
		ret = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
			(const void *)&imreq, sizeof(imreq));
		if (ret < 0)
		{
			err("src: multicast route missing on the interface");
			goto err;
		}
	}
	else if ((af == AF_INET6) && IN6_IS_ADDR_MULTICAST(&in6addr))
	{
		struct ipv6_mreq imreq;
		memset(&imreq, 0, sizeof(struct ip_mreq));
		memcpy(&imreq.ipv6mr_multiaddr.s6_addr, &in6addr, sizeof(struct in6_addr));
		imreq.ipv6mr_interface = 0; // use DEFAULT interface

		// JOIN multicast group on default interface
		ret = setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
			(const void *)&imreq, sizeof(imreq));
		if (ret < 0)
		{
			err("src: multicast route missing on the interface");
			goto err;
		}
	}

	if (af == AF_INET)
	{
		memset(&ctx->saddr, 0, sizeof(ctx->saddr));
		ctx->saddr.sin_family = PF_INET;
		ctx->saddr.sin_addr.s_addr = inaddr;
		ctx->saddr.sin_port = htons(iport);
		ctx->addr = (struct sockaddr*)&ctx->saddr;
		ctx->addrlen = sizeof(ctx->saddr);
	}
	else
	{
		memset(&ctx->saddr6, 0, sizeof(ctx->saddr6));
		ctx->saddr6.sin6_family = PF_INET6;
		memcpy(&ctx->saddr6.sin6_addr, &in6addr, sizeof(struct in6_addr));
		ctx->saddr6.sin6_port = htons(iport);
		ctx->addr = (struct sockaddr*)&ctx->saddr6;
		ctx->addrlen = sizeof(ctx->saddr6);
	}
	return sock;
err:
	close(sock);
	return -1;
}

static src_ctx_t *_src_init(player_ctx_t *player, const char *url, const char *mime)
{
	const char *protocol = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *path = NULL;
	const char *search = NULL;

	void *value = utils_parseurl(url, &protocol, &host, &port, &path, &search);

	/// rfc3551
	int iport = 5004;
	if (port != NULL)
		iport = atoi(port);

	if (search != NULL)
	{
		mime = strstr(search, "mime=");
		if (mime != NULL)
		{
			mime += 5;
		}
	}
	mime = utils_mime2mime(mime);

	if (protocol == NULL)
	{
		free(value);
		return NULL;
	}
	demux_t *demux = demux_build(player, url, mime);
	if (demux == NULL)
	{
		free(value);
		return NULL;
	}

	src_ctx_t *ctx = NULL;
	ctx = calloc(1, sizeof(*ctx));

	ctx->player = player;
	if (host == NULL || !strcmp(host, "mdns"))
	{
		host = NULL;
	}
	if (host != NULL)
	{
		int sock = _src_connect(ctx, host, iport);
		ctx->sock = sock;
		ctx->host = strdup(host);
		ctx->port = iport;
	}
#ifdef DEMUX_PASSTHROUGH
	ctx->demux = demux;
#endif
	ctx->mime = mime;
#ifdef UDP_DUMP
	ctx->dumpfd = open("udp_dump.stream", O_RDWR | O_CREAT, 0644);
#endif
	dbg("src: %s", src_udp->name);

	free(value);
	return ctx;
}

static int _src_read(src_ctx_t *ctx, unsigned char *buff, int len)
{
	int ret;
	fd_set rfds;
	int maxfd = ctx->sock;
	FD_ZERO(&rfds);
	FD_SET(ctx->sock, &rfds);
	ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
	if (ret != 1)
	{
		err("udp select %d %s", ret, strerror(errno));
		return -1;
	}
	int length;
	ret = ioctl(ctx->sock, FIONREAD, &length);
	if (length > ctx->out->ctx->size)
		warn("src: fionread %d > %ld", length, ctx->out->ctx->size);
	if (length == 0)
		warn("src: fionread %d", length);
#ifdef UDP_MARKER
	if (length == 4)
	{
		unsigned long marker = 0;
		ret = recvfrom(ctx->sock, (char *)&marker, sizeof(marker),
				0, ctx->addr, &ctx->addrlen);
		dbg("udp: marker %lx", marker);
		return src_read(ctx, buff, len);
	}
	else
#endif
	{
		ret = recvfrom(ctx->sock, buff, len,
				0, ctx->addr, &ctx->addrlen);
		src_dbg("src: play %d", ret);
		if (ret < 0)
		{
			ctx->state = STATE_ERROR;
			err("src: udp reception error %s", strerror(errno));
		}
		else if (ret == 0)
		{
			warn("src: udp end of stream");
			ctx->state = STATE_ERROR;
		}
		else
		{
#ifdef UDP_DUMP
			if (ctx->dumpfd > 0)
			{
				write(ctx->dumpfd, buff, ret);
			}
#endif
			src_dbg("src: play %d", ret);
		}
	}
	return ret;
}

static void *_src_thread(void *arg)
{
	src_ctx_t *ctx = (src_ctx_t *)arg;

	int ret;
#ifdef USE_REALTIME
	cpu_set_t cpuset;
	pthread_t self = pthread_self();
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);

	ret = pthread_setaffinity_np(self, 1, &cpuset);
	if (ret != 0)
		err("src: CPUC affinity error: %s", strerror(errno));
#endif
#ifdef UDP_MARKER
	warn("src: udp marker is ON");
#endif
	while (ctx->state != STATE_ERROR)
	{
		unsigned char *buff = ctx->out->ops->pull(ctx->out->ctx);
		if (buff == NULL)
		{
			ctx->state = STATE_ERROR;
			break;
		}
		ret = _src_read(ctx, buff, ctx->out->ctx->size);
		if (ret > 0)
		{
			ctx->out->ops->push(ctx->out->ctx, ret, NULL);
		}
	}
	dbg("src: thread end");
	ctx->out->ops->flush(ctx->out->ctx);
#ifndef DEMUX_PASSTHROUGH
	const src_t src = { .ops = src_udp, .ctx = ctx};
	event_end_es_t event = {.pid = ctx->pid, .src = &src, .decoder = ctx->estream};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_END_ES, (void *)&event);
		listener = listener->next;
	}
#endif
	return NULL;
}

static int _src_wait(src_ctx_t *ctx)
{
#ifdef DEMUX_PASSTHROUGH
	ctx->demux->ops->run(ctx->demux->ctx);
	ctx->out = ctx->demux->ops->jitter(ctx->demux->ctx, JITTE_HIGH);
#else
	const src_t src = { .ops = src_udp, .ctx = ctx};
	event_new_es_t event = {.pid = 0, .src = &src, .mime = ctx->mime, .jitte = JITTE_HIGH};
	event_decode_es_t event_decode = {.src = &src};
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		listener->cb(listener->arg, SRC_EVENT_NEW_ES, (void *)&event);
		event_decode.pid = event.pid;
		event_decode.decoder = event.decoder;
		listener->cb(listener->arg, SRC_EVENT_DECODE_ES, (void *)&event_decode);
		listener = listener->next;
	}
#endif
	return 0;
}

static int _src_start(src_ctx_t *ctx)
{
#ifdef USE_REALTIME
	int ret;

	pthread_attr_t attr;
	struct sched_param params;

	pthread_attr_init(&attr);

	ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	if (ret < 0)
		err("setdetachstate error %s", strerror(errno));
	ret = pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS);
	if (ret < 0)
		err("setscope error %s", strerror(errno));
	ret = pthread_attr_setschedpolicy(&attr, SRC_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = SRC_PRIORITY;
	ret = pthread_attr_setschedparam(&attr, &params);
	if (ret < 0)
		err("setschedparam error %s", strerror(errno));
	if (getuid() == 0)
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	else
	{
		warn("run server as root to use realtime");
		ret = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
	}
	if (ret < 0)
		err("setinheritsched error %s", strerror(errno));
	pthread_create(&ctx->thread, &attr, _src_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, _src_thread, ctx);
#endif
	return 0;
}

static int _src_run(src_ctx_t *ctx)
{
	_src_wait(ctx);
	if (ctx->sock > 0)
		_src_start(ctx);
	return 0;
}

static const char *_src_mime(src_ctx_t *ctx, int index)
{
	if (index == -1)
		return mime_octetstream;
	if (ctx->demux->ctx && ctx->demux->ops->mime)
		return ctx->demux->ops->mime(ctx->demux->ctx, index);
	return ctx->mime;
}

static void _src_eventlistener(src_ctx_t *ctx, event_listener_cb_t cb, void *arg)
{
#ifdef DEMUX_PASSTHROUGH
	ctx->demux->ops->eventlistener(ctx->demux->ctx, cb, arg);
#else
	event_listener_t *listener = calloc(1, sizeof(*listener));
	listener->cb = cb;
	listener->arg = arg;
	if (ctx->listener == NULL)
		ctx->listener = listener;
	else
	{
		/**
		 * add listener to the end of the list. this allow to call
		 * a new listener with the current event when the function is
		 * called from a callback
		 */
		event_listener_t *previous = ctx->listener;
		while (previous->next != NULL) previous = previous->next;
		previous->next = listener;
	}
#endif
}

static int _src_attach(src_ctx_t *ctx, long index, decoder_t *decoder)
{
	int ret = 0;
	dbg("src: attach");
#ifdef DEMUX_PASSTHROUGH
	ret = ctx->demux->ops->attach(ctx->demux->ctx, index, decoder);
	ctx->out = ctx->demux->ops->jitter(ctx->demux->ctx, JITTE_HIGH);
#else
	ctx->estream = decoder;
	ctx->out = decoder->ops->jitter(decoder->ctx, JITTE_HIGH);
#endif
#ifndef UDP_THREAD
	dbg("src: add producter to %s", ctx->out->ctx->name);
	ctx->out->ctx->produce = (produce_t)src_read;
	ctx->out->ctx->producter = (void *)ctx;
#endif
	return ret;
}

static decoder_t *_src_estream(src_ctx_t *ctx, long index)
{
#ifdef DEMUX_PASSTHROUGH
	return ctx->demux->ops->estream(ctx->demux->ctx, index);
#else
	return ctx->estream;
#endif
}

#ifdef TINYSVCMDNS
static void _src_mdns(src_ctx_t *ctx, const char *host, struct rr_entry *entry)
{
	warn("!!!!!!!!!!!!!! coucou !!!!!!!!!!!!!!!!!!");
	warn("entry %p", entry);
	if (entry != NULL)
	{
		char *namestr = nlabel_to_str(entry->name);
		warn("tinysvcmdns: name = %s type = %s", namestr, rr_get_type_name(entry->type));
	}
#if 0
	const char *host = "224.0.0.1";
	int iport = 4400;
	int sock = src_connect(ctx, host, iport);
	if (ctx->sock > 0)
		src_start(ctx);
#endif
}
#else
#define _src_mdns NULL
#endif

static void _src_destroy(src_ctx_t *ctx)
{
#ifdef UDP_THREAD
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
#endif
#ifdef DEMUX_PASSTHROUGH
	ctx->demux->ops->destroy(ctx->demux->ctx);
#else
	if (ctx->estream != NULL)
		ctx->estream->ops->destroy(ctx->estream->ctx);
	event_listener_t *listener = ctx->listener;
	while (listener)
	{
		event_listener_t *next = listener->next;
		free(listener);
		listener = next;
	}
#endif
#ifdef UDP_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	close(ctx->sock);
	free(ctx->host);
	free(ctx);
}

static const char *_src_medium()
{
	return mime_octetstream;
}

const src_ops_t *src_udp = &(const src_ops_t)
{
	.name = "udp",
	.protocol = "udp://|rtp://",
	.medium = _src_medium,
	.init = _src_init,
	.run = _src_run,
	.eventlistener = _src_eventlistener,
	.attach = _src_attach,
	.estream = _src_estream,
	.mdns = _src_mdns,
	.destroy = _src_destroy,
};
