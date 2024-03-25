#ifndef __RTP_H__
#define __RTP_H__

#include <stdint.h>
#include <arpa/inet.h>

#define RTP_HEARTBEAT_TIMELAPS 10000000 // ns <= 100 Hz
struct rtpbits {
    uint32_t     cc:4;            // number of CSRC identifiers: 0
    uint32_t     x:1;             // has extension header: 0
    uint32_t     p:1;             // is there padding appended: 0
    uint32_t     v:2;             // version: 2
    uint32_t     pt:7;            // payload type: 14 for MPEG audio
    uint32_t     m:1;             // marker: 0
    uint32_t     seqnum:16;     // sequence number: start random
};
struct rtpheader_s {           // in network byte order
    struct rtpbits b;
    uint32_t     timestamp;       // start: random
    uint32_t     ssrc;            // random
    uint32_t     csrc[1];         // ssrc of all sources mixed
};

typedef struct rtpheader_s rtpheader_t;

struct demux_ctx_s;
typedef struct demux_ctx_s demux_ctx_t;
extern void demux_rtp_addprofile(demux_ctx_t *ctx, char pt, const char *mime);

typedef struct rtpext_s rtpext_t;
struct rtpext_s
{
	uint16_t extid;
	uint16_t extlength;
};

typedef struct rtpext_pcm_s rtpext_pcm_t;
struct rtpext_pcm_s
{
	jitter_format_t format;
	uint16_t samplerate;
};

#define PUTVCTRL_PT 0x76
#define PUTVCTRL_VERSION 0x01
#define PUTVCTRL_ID_STATE	0x01
#define PUTVCTRL_ID_VOLUME	0x02
typedef struct rtpext_putvctrl_cmd_s rtpext_putvctrl_cmd_t;
struct rtpext_putvctrl_cmd_s
{
	uint8_t id;
	uint8_t len;
	uint16_t data;
};
typedef struct rtpext_putvctrl_s rtpext_putvctrl_t;
struct rtpext_putvctrl_s
{
	uint8_t version;
	uint8_t ncmds;
	uint16_t reserved;
	rtpext_putvctrl_cmd_t cmd;
};

extern const char *rtp_service;// _rtp._udp
#endif
