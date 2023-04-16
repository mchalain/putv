/*****************************************************************************
 * sink_udp.c
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
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <pthread.h>

#include <unistd.h>
#include <poll.h>
#include <sched.h>

#include "player.h"
#include "mux.h"
#include "encoder.h"
#include "jitter.h"
#include "unix_server.h"
typedef struct sink_s sink_t;
typedef struct sink_ctx_s sink_ctx_t;
struct sink_ctx_s
{
	player_ctx_t *player;
	jitter_t *in;
	event_listener_t *listener;

	const char *filepath;
	struct sockaddr_in saddr;
	pthread_t thread;
	state_t state;
	unsigned int samplerate;
	int samplesize;
	int nchannels;
	int sock;
	int counter;
	char *sink_txt[10];
#ifdef MUX
	mux_t *mux;
#endif
	const encoder_ops_t *encoder;
#ifdef UDP_DUMP
	int dumpfd;
#endif
};
#define SINK_CTX
#include "sink.h"
#include "media.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define sink_dbg(...)

#define SINK_POLICY REALTIME_SCHED
#define SINK_PRIORITY 65

static const char *jitter_name = "udp socket";
static sink_ctx_t *sink_init(player_ctx_t *player, const char *url)
{
	int ret = 0;
	const encoder_ops_t *encoder = encoder_lame;

	char *protocol = NULL;
	char *host = NULL;
	char *port = NULL;
	char *path = NULL;
	char *search = NULL;
	in_addr_t if_addr = INADDR_ANY;

	char *value = utils_parseurl(url, &protocol, &host, &port, &path, &search);

	if (protocol == NULL)
	{
		free(value);
		return NULL;
	}
	int rtp = !strcmp(protocol, "rtp");
	if (!rtp && strcmp(protocol, "udp"))
	{
		free(value);
		return NULL;
	}

	int iport = 4400;
	if (port != NULL)
		iport = atoi(port);

	if (search != NULL)
	{
		const char *nif;
		nif = strstr(search, "if=");
		if (nif != NULL)
		{
			nif += 3;
			struct ifaddrs *ifa_list;
			struct ifaddrs *ifa_main;

			if (getifaddrs(&ifa_list) == 0)
			{
				for (ifa_main = ifa_list; ifa_main != NULL; ifa_main = ifa_main->ifa_next)
				{
					if ((nif != NULL) && strcmp(nif, ifa_main->ifa_name) != 0)
						continue;
				}
				if (ifa_main != NULL)
				{
					if_addr = ((struct sockaddr_in *)ifa_main->ifa_addr)->sin_addr.s_addr;
				}
			}
		}
		const char *mime = strstr(search, "mime=");
		if (mime != NULL)
		{
			encoder = encoder_check(mime + 5);
		}
	}

	int count = 2;
	jitter_format_t format = SINK_BITSSTREAM;
	sink_ctx_t *ctx = NULL;
	int sock;
	int mtu;

	sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	//TODO: try IPPROTO_UDPLITE

	struct sockaddr_in saddr;
	memset(&saddr, 0, sizeof(struct in_addr));
	saddr.sin_family = PF_INET;
	saddr.sin_port = htons(0); // Use the first free port
	saddr.sin_addr.s_addr = htonl(if_addr); // bind socket to any interface

	ret = bind(sock, (struct sockaddr *)&saddr, sizeof(saddr));

	if (ret != -1)
	{
		unsigned long longaddress = inet_addr(host);

		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_addr.sa_family = AF_INET;
		int i;
		for ( i = 0; i < 5; i++)
		{
			ifr.ifr_ifindex = i;
			ret = ioctl(sock, SIOCGIFNAME, &ifr);
			if (ret == 0)
			{
				ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
				if (ret)
					continue;
				if ((ifr.ifr_flags & IFF_LOOPBACK))
				{
					continue;
				}
				if (IN_MULTICAST(longaddress) && !(ifr.ifr_flags & IFF_MULTICAST))
					continue;
				if (!(ifr.ifr_flags & IFF_RUNNING))
				{
					break;
				}
			}
		}
		ret = ioctl(sock, SIOCGIFMTU, &ifr);
		mtu = (ret == -1)?1500:ifr.ifr_mtu;

		int value=1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
		//check if the addrese is for broadcast diffusion
		if (htonl(longaddress) > 0xff000000)
		{
			warn("sink: udp broadcasting");
			ret = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value));
			if (ret != -1)
				err("sync: udp broadcast error %s", strerror(errno));
			if (! ifr.ifr_flags & IFF_BROADCAST)
				err("sync: udp broadcast interface not supported");
		}
		// check if the address is for multicast diffusion
		else if (IN_MULTICAST(htonl(longaddress)))
		{
			warn("sink: udp multicasting");
			if (! ifr.ifr_flags & IFF_MULTICAST)
				err("sync: udp multicast interface not supported");

			struct in_addr iaddr;
			// set content of struct saddr and imreq to zero
			memset(&iaddr, 0, sizeof(struct in_addr));
			iaddr.s_addr = if_addr;

			// Set the outgoing interface to DEFAULT
			ret = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
							sizeof(struct in_addr));
			if (ret != 0)
				warn("sink: not allowed to change interface");

			unsigned char ttl = 3;
			// Set multicast packet TTL to 3; default TTL is 1
			ret = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
							sizeof(unsigned char));
			if (ret != 0)
				warn("sink: not allowed to set TTL");

			unsigned char one = 1;
			// send multicast traffic to myself too
			ret = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &one,
							sizeof(unsigned char));
			if (ret != 0)
				warn("sink: not allowed to make a loop on data sending");
		}

		saddr.sin_family = PF_INET;
		saddr.sin_addr.s_addr = longaddress;
		saddr.sin_port = htons(iport);

	}
	if (sock > 0 && ret == 0)
	{

		ctx = calloc(1, sizeof(*ctx));

		ctx->player = player;
		ctx->sock = sock;
		ctx->encoder = encoder;
		memcpy(&ctx->saddr, &saddr, sizeof(saddr));
		int i = 0;
		if (asprintf(&ctx->sink_txt[i++], "h=%s", host) < 0)
		{
			i--; ctx->sink_txt[i++] = NULL;
		}
		if (asprintf(&ctx->sink_txt[i++], "p=%i", iport) < 0)
		{
			i--; ctx->sink_txt[i++] = NULL;
		}

		unsigned int size = mtu;
		jitter_t *jitter = jitter_init(JITTER_TYPE_SG, jitter_name, 6, size);
#ifdef USE_REALTIME
		jitter->ops->lock(jitter->ctx);
#endif
		jitter->ctx->frequence = 0;
		jitter->ctx->thredhold = 3;
		jitter->format = format;
		ctx->in = jitter;
#ifdef MUX
		ctx->mux = mux_build(player, protocol, search);
#endif
#ifdef UDP_DUMP
		ctx->dumpfd = open("udp_dump.stream", O_RDWR | O_CREAT, 0644);
#endif
	}
	free(value);

	return ctx;
}

static sink_ctx_t *sink_init_rtp(player_ctx_t *player, const char *url)
{
	return sink_init(player, url);
}

static unsigned int sink_attach(sink_ctx_t *ctx, const char *mime)
{
#ifdef MUX
	return ctx->mux->ops->attach(ctx->mux->ctx, mime);
#else
	return 0;
#endif
}

static jitter_t *sink_jitter(sink_ctx_t *ctx, unsigned int index)
{
#ifdef MUX
	return ctx->mux->ops->jitter(ctx->mux->ctx, index);
#else
	return ctx->in;
#endif
}

static const encoder_ops_t *sink_encoder(sink_ctx_t *ctx)
{
	return ctx->encoder;
}

static void *sink_thread(void *arg)
{
	sink_ctx_t *ctx = (sink_ctx_t *)arg;
	int run = 1;

	/* start decoding */
	dbg("sink: thread run");
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
	warn("sink: udp marker is ON");
#endif
	while (run)
	{
		unsigned char *buff = ctx->in->ops->peer(ctx->in->ctx, NULL);
		if (buff == NULL)
		{
			run = 0;
			break;
		}

		size_t length = ctx->in->ops->length(ctx->in->ctx);

#ifdef UDP_MARKER
		static unsigned long marker = 0;
		ret = sendto(ctx->sock, (char *)&marker, sizeof(marker), MSG_NOSIGNAL| MSG_DONTWAIT,
				(struct sockaddr *)&ctx->saddr, sizeof(ctx->saddr));
		dbg("send %lx", marker);
		marker++;
#endif
		ret = 0;
		size_t len = length;
		while (len > 0)
		{
			int maxfd = ctx->sock;
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(ctx->sock, &wfds);
			ret = select(maxfd + 1, NULL, &wfds, NULL, NULL);
			if (ret > 0 && FD_ISSET(ctx->sock, &wfds))
			{
				ret = sendto(ctx->sock, buff, len, MSG_NOSIGNAL| MSG_DONTWAIT,
						(struct sockaddr *)&ctx->saddr, sizeof(ctx->saddr));
				sink_dbg("udp: send %d", ret);
			}
			if (ret < 0)
			{
				if (errno == EAGAIN)
					continue;
				err("sink: udp send error %s", strerror(errno));
				close(ctx->sock);
				run = 0;
				break;
			}
			else
			{
#ifdef UDP_DUMP
				write(ctx->dumpfd, buff, ret);
#endif
			}
			len -= ret;
			buff += ret;
			sched_yield();
		}
		if (len == 0)
		{
			sink_dbg("sink: udp play %d", ret);
			ctx->counter++;
		}

#ifdef DEBUG
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		sink_dbg("sink: boom %d %lu.%09lu", ctx->counter, now.tv_sec, now.tv_nsec);
#endif
		ctx->in->ops->pop(ctx->in->ctx, length);
	}
	dbg("sink: thread end");
	return NULL;
}

static int sink_run(sink_ctx_t *ctx)
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
	ret = pthread_attr_setschedpolicy(&attr, SINK_POLICY);
	if (ret < 0)
		err("setschedpolicy error %s", strerror(errno));
	params.sched_priority = SINK_PRIORITY;
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
	pthread_create(&ctx->thread, &attr, sink_thread, ctx);
	pthread_attr_destroy(&attr);
#else
	pthread_create(&ctx->thread, NULL, sink_thread, ctx);
#endif
#ifdef MUX
	ctx->mux->ops->run(ctx->mux->ctx, ctx->in);
#endif

	return 0;
}

static const char *sink_service(sink_ctx_t *ctx, int *port, const char **txt[])
{
	*port = htons(ctx->saddr.sin_port);
	*txt = ctx->sink_txt;
	return "_rtp._udp";
}

static void sink_destroy(sink_ctx_t *ctx)
{
	if (ctx->thread)
		pthread_join(ctx->thread, NULL);
	jitter_destroy(ctx->in);
	int i = 0;
	while (ctx->sink_txt[i] != NULL)
		free(ctx->sink_txt[i++]);
#ifdef UDP_DUMP
	if (ctx->dumpfd > 0)
		close(ctx->dumpfd);
#endif
	free(ctx);
}

const sink_ops_t *sink_udp = &(sink_ops_t)
{
	.name = "udp",
	.default_ = "udp://127.0.0.1:440",
	.init = sink_init,
	.jitter = sink_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = sink_run,
	.service = sink_service,
	.destroy = sink_destroy,
};

const sink_ops_t *sink_rtp = &(sink_ops_t)
{
	.name = "rtp",
	.default_ = "rtp://127.0.0.1:440",
	.init = sink_init_rtp,
	.jitter = sink_jitter,
	.attach = sink_attach,
	.encoder = sink_encoder,
	.run = sink_run,
	.service = sink_service,
	.destroy = sink_destroy,
};
