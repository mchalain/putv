/*
 * tinysvcmdns - a tiny MDNS implementation for publishing services
 * Copyright (C) 2011 Darell Tan
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>

#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <strings.h>
#include <ifaddrs.h>

#include "mdns.h"
#include "mdnsd.h"

struct mdns_service *svc;
struct mdnsd *svr;

/*---------------------------------------------------------------------------*/
#define MAX_INTERFACES 256
#define DEFAULT_INTERFACE 1

static in_addr_t get_localhost(char **name)
{
	struct ifaddrs *ifap, *ifa;

	if (name) {
		*name = malloc(256);
		gethostname(*name, 256);
	}

	if (getifaddrs(&ifap) != 0) return INADDR_ANY;

	/* cycle through available interfaces */
	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		/* Skip loopback, point-to-point and down interfaces,
		 * except don't skip down interfaces
		 * if we're trying to get a list of configurable interfaces. */
		if ((ifa->ifa_flags & IFF_LOOPBACK) ||
			(!( ifa->ifa_flags & IFF_UP))) {
			continue;
		}
		if (ifa->ifa_addr->sa_family == AF_INET) {
			/* We don't want the loopback interface. */
			if (((struct sockaddr_in *)(ifa->ifa_addr))->sin_addr.s_addr ==
				htonl(INADDR_LOOPBACK)) {
				continue;
			}
			return ((struct sockaddr_in *)(ifa->ifa_addr))->sin_addr.s_addr;
			break;
		}
	}
	freeifaddrs(ifap);

	return INADDR_ANY;
}


/*---------------------------------------------------------------------------*/
static int print_usage(void) {
	printf("[host <ip>] <identity> <type> <port> <txt> [txt] ... [txt]\n");
	return 1;
}


/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	mdnsd_stop(svr);
	exit(0);
}


/*---------------------------------------------------------------------------*/
/*																			 */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
	char type[255];
	int port;
	const char **txt;
	struct in_addr host;
	char *hostname;
	int opt = 0;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGPIPE)
	signal(SIGPIPE, SIG_IGN);
#endif
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	host.s_addr = get_localhost(&hostname);
	hostname = realloc(hostname, strlen(hostname) + strlen(".local") + 1);
	strcat(hostname, ".local");

	if (argc < 2) {
		return print_usage();
	}

	if (!strcasecmp(argv[1], "host")) {
		host.s_addr = inet_addr(argv[2]);
		opt = 2;
	}

	if (host.s_addr == INADDR_ANY) {
		printf("cannot find host address\n");
		free(hostname);
		return print_usage();
	}

	if (argc < 5+opt) return print_usage();

	port = atoi(argv[3+opt]);

	svr = mdnsd_start(&host);
	if (svr == NULL) return print_usage();

	txt = malloc((argc - 4 + 1 - opt) * sizeof(char**));
	memcpy(txt, argv + 4 + opt, (argc - 4 - opt) * sizeof(char**));
	txt[argc - 4 - opt] = NULL;

	mdnsd_set_hostname(svr, hostname, &host);

	sprintf(type, "%s.local", argv[2 + opt]);

	printf("host     : %s\nidentity : %s\ntype     : %s\n"
		   "ip       : %s\nport     : %u\n",
			hostname, argv[1 + opt], type, inet_ntoa(host), port);

	free(hostname);

	svc = mdnsd_register_svc(svr, argv[1 + opt], type, port, NULL, txt);
	// or, to remove service call: mdns_service_remove(svr, svc);
	mdns_service_destroy(svc);

	pause();

	mdnsd_stop(svr);

	return 0;
}

