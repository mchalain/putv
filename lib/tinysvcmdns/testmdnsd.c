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

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#include <stdio.h>
#include "mdns.h"
#include "mdnsd.h"

static in_addr_t search_host(void)
{
	in_addr_t addr = 0;
	struct ifaddrs *ifa_list;
	struct ifaddrs *ifa_main;

	if (getifaddrs(&ifa_list) < 0) {
		DEBUG_PRINTF("getifaddrs() failed");
		return 0;
	}

	for (ifa_main = ifa_list; ifa_main != NULL; ifa_main = ifa_main->ifa_next)
	{
		if ((ifa_main->ifa_flags & IFF_LOOPBACK) || !(ifa_main->ifa_flags & IFF_MULTICAST))
			continue;
		if (ifa_main->ifa_addr && ifa_main->ifa_addr->sa_family == AF_INET)
		{
			addr = ((struct sockaddr_in *)ifa_main->ifa_addr)->sin_addr.s_addr;
			break;
		}
	}
	return addr;
}
int lookup_service(void *data, const char *name, struct lookup_arg args[], int nargs)
{
	struct in_addr in = {0};
	printf("LOOKUP found %s\n", name);
	for (int i = 0; i < nargs; i++) {
		switch (args[i].type) {
			case lookup_hostname:
				printf("\thost: %s\n", (char *)args[i].val);
			break;
			case lookup_port:
				printf("\tport: %ld\n", (long int)args[i].val);
			break;
			case lookup_address:
				in.s_addr = (long int)args[i].val;
				printf("\taddr: %s\n", inet_ntoa(in));
			break;
			case lookup_other:
				printf("\tother: %s\n", (char *)args[i].val);
			break;
		}
	}
	return 0;
}

int main(int argc, char *argv[]) {
	// create host entries
	char *hostname = "some-random-host.local";

	struct mdnsd *svr = mdnsd_start(NULL);
	if (svr == NULL) {
		printf("mdnsd_start() error\n");
		return 1;
	}

	printf("mdnsd_start OK. press ENTER to add hostname %s & service\n", hostname);
	getchar();

	struct in_addr v4addr;
	v4addr.s_addr = search_host();
	mdnsd_set_hostname(svr, hostname, &v4addr);

	// Add all alternative IP addresses for this host
	struct rr_entry *a2_e = NULL;
	v4addr.s_addr = inet_addr("192.168.0.10");
	a2_e = rr_create_a(create_nlabel(hostname), &v4addr);
	mdnsd_add_rr(svr, a2_e);

	struct rr_entry *aaaa_e = NULL;

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_flags = AI_NUMERICHOST;
	struct addrinfo* results;
	getaddrinfo(
		"fe80::e2f8:47ff:fe20:28e0",
		NULL,
		&hints,
		&results);
	struct sockaddr_in6* addr = (struct sockaddr_in6*)results->ai_addr;

	aaaa_e = rr_create_aaaa(create_nlabel(hostname), &addr->sin6_addr);
	freeaddrinfo(results);

	mdnsd_add_rr(svr, aaaa_e);

	const char *txt[] = {
		"name=toto", 
		NULL
	};
	struct mdns_service *svc = mdnsd_register_svc(svr, "mytest", 
									"_http._tcp.local", 8080, NULL, txt);

	printf("added service and hostname. press ENTER to search hostname for %s\n", hostname);
	getchar();

	struct in_addr in;
	in.s_addr = mdnsd_search_hostname(svr, hostname);
	printf("hostname %s found. press ENTER to search _http._tcp.local\n", inet_ntoa(in));
	getchar();

	void * search = mdnsd_search(svr, "_http._tcp.local", lookup_service, NULL);
	printf("press ENTER to remove service\n");
	getchar();
	mdnsd_unsearch(svr, search);

	mdns_service_remove(svr, svc);

	mdns_service_destroy(svc);
	printf("removed service. press ENTER to search hostname for %s\n", hostname);
	getchar();

	in.s_addr = mdnsd_search_hostname(svr, hostname);
	printf("hostname %s found. press ENTER to exit\n", inet_ntoa(in));
	getchar();

	mdnsd_stop(svr);

	return 0;
}

