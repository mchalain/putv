#ifndef __RTP_H__
#define __RTP_H__

#include <stdint.h>
#include <arpa/inet.h>

struct rtpbits {
    uint32_t     seqnum:16;     // sequence number: start random
    uint32_t     pt:7;            // payload type: 14 for MPEG audio
    uint32_t     m:1;             // marker: 0
    uint32_t     cc:4;            // number of CSRC identifiers: 0
    uint32_t     x:1;             // has extension header: 0
    uint32_t     p:1;             // is there padding appended: 0
    uint32_t     v:2;             // version: 2
};

struct rtpheader_s {           // in network byte order
    struct rtpbits b;
    uint32_t     timestamp;       // start: random
    uint32_t     ssrc;            // random
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

#endif
