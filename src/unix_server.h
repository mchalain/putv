#ifndef __UNIX8SERVER_H__
#define __UNIX8SERVER_H__

typedef struct thread_server_s thread_server_t;
typedef struct thread_info_s thread_info_t;
struct thread_info_s
{
	int sock;
	void *userctx;
	thread_server_t *server;
	thread_info_t *next;
};

typedef int (*client_routine_t)(thread_info_t *info);
int unixserver_run(client_routine_t routine, void *userctx, const char *socketpath);
void unixserver_remove(thread_info_t *info);

#endif
