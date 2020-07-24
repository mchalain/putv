/*****************************************************************************
 * cmds_tinysvmdns.c
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <bits/local_lim.h>
#include <sys/utsname.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>

#include <pthread.h>

#include "mdns.h"
#include "mdnsd.h"

#include "player.h"
typedef struct cmds_ctx_s cmds_ctx_t;
struct cmds_ctx_s
{
	player_ctx_t *player;
	char *appname;
	int run;
	struct mdnsd *svr;
	struct mdns_service *svc[2];
	pthread_t thread;
	int onchangeid;
};
#define CMDS_CTX
#include "cmds.h"
#include "media.h"
#include "sink.h"
#include "src.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static const char *default_appname = "PumpUpTheVolume";
static cmds_ctx_t *cmds_tinysvcmdns_init(player_ctx_t *player, void *arg)
{
	const char *nif = NULL;
	const char *appname = NULL;
	const char **txt = arg;
	while (*txt != NULL)
	{
		nif = strstr(*txt, "if=");
		if (nif != NULL)
		{
			nif += 3;
			if (nif[0] == '\0')
				nif = NULL;
		}
		appname = strstr(*txt, "an=");
		if (appname != NULL)
		{
			appname += 3;
			if (appname[0] == '\0')
				appname = NULL;
		}
		txt++;
	}

	char hostname[HOST_NAME_MAX + 1 + 6];
	gethostname(hostname, HOST_NAME_MAX);
	if (strlen(hostname) == 0)
	{
		struct utsname sysinfo;
		uname(&sysinfo);
		snprintf(hostname, HOST_NAME_MAX + 1 + 6, "%s.local", sysinfo.nodename);
	}
	else
		strcat(hostname, ".local");

	struct ifaddrs *ifa_list;
	struct ifaddrs *ifa_main;

	if (getifaddrs(&ifa_list) < 0) {
		warn("tinysvcmdns: getifaddrs() failed");
		return NULL;
	}

	struct mdnsd *svr = NULL;
	for (ifa_main = ifa_list; ifa_main != NULL; ifa_main = ifa_main->ifa_next)
	{
		if ((nif != NULL) && strcmp(nif, ifa_main->ifa_name) != 0)
			continue;
		if ((ifa_main->ifa_flags & IFF_LOOPBACK) || !(ifa_main->ifa_flags & IFF_MULTICAST))
			continue;
		if (ifa_main->ifa_addr && ifa_main->ifa_addr->sa_family == AF_INET)
		{
			if (svr == NULL)
			{
				svr = mdnsd_start(&((struct sockaddr_in *)ifa_main->ifa_addr)->sin_addr);
			}
			if (svr == NULL) {
				err("tinysvcmdns: start error %s\n", strerror(errno));
				return NULL;
			}

			in_addr_t main_ip = ((struct sockaddr_in *)ifa_main->ifa_addr)->sin_addr.s_addr;

			mdnsd_set_hostname(svr, hostname, main_ip); // TTL should be 120 seconds
			break;
		}
#ifdef IPV6
		else if (ifa_main->ifa_addr && ifa_main->ifa_addr->sa_family == AF_INET6)
		{
			if (svr == NULL)
				svr = mdnsd_start();
			if (svr == NULL) {
				err("mdnsd_start() error\n");
				return NULL;
			}

			struct in6_addr *addr = &((struct sockaddr_in6 *)ifa_main->ifa_addr)->sin6_addr;

			mdnsd_set_hostname_v6(svr, hostname, addr); // TTL should be 120 seconds
			break;
		}
#endif
	}

	if (ifa_main == NULL)
	{
		err("tinysvcmdns: ipv4 or ipv6 interface different of loopback not found");
		return NULL;
	}

	struct ifaddrs *ifa_other;
	for (ifa_other = ifa_list; ifa_other != NULL; ifa_other = ifa_other->ifa_next)
	{
		if ((ifa_other->ifa_flags & IFF_LOOPBACK) || !(ifa_other->ifa_flags & IFF_MULTICAST))
			continue;
		if (ifa_other == ifa_main)
			continue;
		switch (ifa_other->ifa_addr->sa_family)
		{
			case AF_INET:
			{
				uint32_t ip = ((struct sockaddr_in *)ifa_other->ifa_addr)->sin_addr.s_addr;
				struct rr_entry *a_e =
				rr_create_a(create_nlabel(hostname), ip); // TTL should be 120 seconds
				mdnsd_add_rr(svr, a_e);
			}
			break;
			case AF_INET6:
			{
				struct in6_addr *addr = &((struct sockaddr_in6 *)ifa_other->ifa_addr)->sin6_addr;
				struct rr_entry *aaaa_e =
				rr_create_aaaa(create_nlabel(hostname), addr); // TTL should be 120 seconds
				mdnsd_add_rr(svr, aaaa_e);
			}
			break;
		}
	}

	freeifaddrs(ifa_list);
	if (appname == NULL)
		appname = default_appname;

	cmds_ctx_t *ctx = NULL;
	ctx = calloc(1, sizeof(*ctx));
	ctx->player = player;
	ctx->svr = svr;

	asprintf(&ctx->appname, "%s@%s", appname, hostname);

	txt = (const char **)arg;
	struct mdns_service *svc = mdnsd_register_svc(svr, ctx->appname,
									"_http._tcp.local", 80, NULL, txt);
	ctx->svc[0] = svc;

	return ctx;
}

static void tinysvcmdns_onchange(void * userctx, event_t event, void *eventarg)
{
	cmds_ctx_t *ctx = (cmds_ctx_t *)userctx;

	switch (event)
	{
		case PLAYER_EVENT_CHANGE:
		{
			event_player_state_t *data = (event_player_state_t *)eventarg;
			src_t *src = player_source(data->playerctx);
			if (src != NULL && src->ops->mdns != NULL)
			{
			}
		}
		break;
	}
}

static int cmds_tinysvcmdns_run(cmds_ctx_t *ctx, sink_t *sink)
{
	if (sink->ops->service != NULL)
	{
		int port;
		char *service;
		const char **txt;
		asprintf(&service, "%s.local", sink->ops->service(sink->ctx, &port, &txt));

		struct mdns_service *svc = mdnsd_register_svc(ctx->svr, ctx->appname,
									service, port, NULL, txt);
		ctx->svc[1] = svc;
	}
	ctx->onchangeid = player_eventlistener(ctx->player,
			tinysvcmdns_onchange, (void *)ctx, "tinysvcmdns");

	return 0;
}

static void cmds_tinysvcmdns_destroy(cmds_ctx_t *ctx)
{
	ctx->run = 0;
	pthread_join(ctx->thread, NULL);
	mdns_service_destroy(ctx->svc[0]);
	if (ctx->svc[1])
		mdns_service_destroy(ctx->svc[1]);
	mdnsd_stop(ctx->svr);
	free(ctx->appname);
	free(ctx);
}

cmds_ops_t *cmds_tinysvcmdns = &(cmds_ops_t)
{
	.init = cmds_tinysvcmdns_init,
	.run = cmds_tinysvcmdns_run,
	.destroy = cmds_tinysvcmdns_destroy,
};
