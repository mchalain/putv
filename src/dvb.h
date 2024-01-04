#ifndef __DVB_H__
#define __DVB_H__

#if 1
typedef union ts_packet_header_s
{
	struct
	{
		unsigned char		sync;
		unsigned char		hpid:5;
		unsigned char		priority:1;
		unsigned char		start_payload:1;
		unsigned char		transport_error:1;
		unsigned char		lpid;
		unsigned char		counter:4;
		unsigned char		payload:1;
		unsigned char		adaption:1;
		unsigned char		scrambling:2;
	}					decoded;
	unsigned char		raw[4];
} ts_packet_header_t;
#else
typedef union ts_packet_header_s
{
	struct
	{
		unsigned long		sync:8;
		unsigned long		transport_error:1;
		unsigned long		start_payload:1;
		unsigned long		priority:1;
		unsigned long		hpid:5;
		unsigned long		lpid:8;
		unsigned long		scrambling:2;
		unsigned long		adaption:1;
		unsigned long		payload:1;
		unsigned long		counter:4;
	}					decoded;
	unsigned char		raw[4];
} ts_packet_header_t;
#endif

typedef union ts_packet_adaption_s
{
	struct
	{
		unsigned char		length:8;
		unsigned char		extension:1;
		unsigned char		private:1;
		unsigned char		splicing:1;
		unsigned char		opcr:1;
		unsigned char		pcr:1;
		unsigned char		priority:1;
		unsigned char		random:1;
		unsigned char		discontinuity:1;
	}					decoded;
	unsigned char		raw[2];
} ts_packet_adaption_t;

typedef union ts_packet_pcr_s
{
	struct
	{
		unsigned char		base[4];
		unsigned char		ext[2];
	}					decoded;
	unsigned char		raw[6];
} ts_packet_pcr_t;

#endif
