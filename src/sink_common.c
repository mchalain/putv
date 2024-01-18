#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "event.h"
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

extern const sink_ops_t *sink_alsa;
extern const sink_ops_t *sink_tinyalsa;
extern const sink_ops_t *sink_file;
extern const sink_ops_t *sink_udp;
extern const sink_ops_t *sink_rtp;
extern const sink_ops_t *sink_unix;
extern const sink_ops_t *sink_pulse;

static sink_t _sink = {0};
sink_t *sink_build(player_ctx_t *player, const char *arg)
{
	const sink_ops_t *sinklist[] = {
#ifdef SINK_ALSA
		sink_alsa,
#endif
#ifdef SINK_FILE
		sink_file,
#endif
#ifdef SINK_TINYALSA
		sink_tinyalsa,
#endif
#ifdef SINK_UDP
		sink_udp,
		sink_rtp,
#endif
#ifdef SINK_UNIX
		sink_unix,
#endif
#ifdef SINK_PULSE
		sink_pulse,
#endif
		NULL
	};

	const sink_ops_t *sinkops = NULL;
	if (!strcmp(arg, "none"))
		return NULL;
	int i = 0;
	const char *protocol = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *path = NULL;
	const char *search = NULL;
	void *data = utils_parseurl(arg, &protocol, &host, &port, &path, &search);
	if (protocol == NULL)
		protocol = arg;
	while (sinklist[i] != NULL)
	{
		sink_dbg("sink: test %s", sinklist[i]->name);
		int len = strlen(sinklist[i]->name);
		if (protocol && !strcmp(sinklist[i]->name, protocol))
			break;
		i++;
	}
	if (sinklist[i] == NULL)
		i = 0;
	sinkops = sinklist[i];
	if (arg[0] == '\0')
		arg = sinklist[i]->default_;
	_sink.ctx = sinkops->init(player, arg);
	if (_sink.ctx == NULL)
		return NULL;
	_sink.ops = sinkops;
	free(data);
	return &_sink;
}
