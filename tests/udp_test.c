#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define DEBUG
#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif
typedef int (*transfer_t)(int sock, int dumpfd, struct sockaddr *addr, int addrlen);
static struct timespec start = {0, 0};

int cli_transfer(int sock, int dumpfd, struct sockaddr *addr, int addrlen)
{
	//int flags = MSG_NOSIGNAL| MSG_DONTWAIT;
	int flags = 0;
	int ret;
	unsigned char buff[1500];
	dbg("wait");
	ret = recvfrom(sock, buff, sizeof(buff),
				flags, addr, &addrlen);
	clockid_t clockid = CLOCK_REALTIME;
	if (start.tv_nsec == 0)
	{
		clock_gettime(clockid, &start);
		dbg("recv %d %s", ret, strerror(errno));
	}
	else
	{
		struct timespec now = {0, 0};
		clock_gettime(clockid, &now);
		now.tv_sec -= start.tv_sec;
		if (now.tv_nsec > start.tv_nsec)
			now.tv_nsec -= start.tv_nsec;
		else
		{
			now.tv_sec -= 1000000000 - start.tv_sec;
			now.tv_sec -= 1;
		}
		dbg("recv %d %lu.%09lu", ret, now.tv_sec, now.tv_nsec);
	}
	if (ret > 0)
	{
		ret = write(dumpfd, buff, ret);
	}
	return (ret!= -1);
}

int srv_transfer(int sock, int dumpfd, struct sockaddr *addr, int addrlen)
{
	int flags = MSG_NOSIGNAL| MSG_DONTWAIT;
	int ret;
	unsigned char buff[1500];
	ret = read(dumpfd, buff, sizeof(buff));
	dbg("read");
	if (ret > 0)
	{
		ret = sendto(sock, buff, ret,
					flags, addr, addrlen);
		dbg("send %d %s", ret, strerror(errno));
		usleep(50000);
	}
	return ret;
}

int main(int argc, char **argv)
{
	transfer_t transfer_cb = cli_transfer;
	int iport = 5004;
	unsigned long longaddress = INADDR_ANY;
	int mode = 'c';
	const char *path = "udp_dump.mp3";

	int opt;
	do
	{
		opt = getopt(argc, argv, "hH:P:SC");
		switch (opt)
		{
			case 'h':
				fprintf(stderr, "%s [-H host] [-P port] [-S|-C] <file>\n", argv[0]);
			return -1;
			case 'H':
				longaddress = inet_addr(optarg);
			break;
			case 'P':
				iport = atoi(optarg);
			break;
			case 'S':
				mode = 's';
				transfer_cb = srv_transfer;
			break;
			case 'C':
				mode = 'c';
				transfer_cb = cli_transfer;
			break;
		}
	} while(opt != -1);
	if (optind < argc)
		path = argv[optind];

	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

	struct sockaddr *addr = NULL;
	int addrlen = 0;
	struct sockaddr_in saddr;

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_family = PF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(iport);
	addr = (struct sockaddr*)&saddr;
	addrlen = sizeof(saddr);

	int ret = bind(sock, addr, addrlen);

	saddr.sin_addr.s_addr = longaddress;

	if (ret == 0)
	{
		int value=1;
		ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
	}

	int dumpfd = open(path, O_RDWR | O_CREAT, 0666);
	int run = 1;
	while (run)
	{
		run = (transfer_cb(sock, dumpfd, addr, addrlen) > 0)? 1: 0;
	}
	close(sock);
	close(dumpfd);
	return 0;
}
