ifeq ($(HEARTBEAT),)
  ifeq ($(SINK_UNIX),y)
    HEARTBEAT=y
  endif
  ifeq ($(SINK_UDP),y)
    HEARTBEAT=y
  endif
endif

bin-y+=putv
putv_SOURCES+=main.c
putv_SOURCES+=daemonize.c
putv_SOURCES+=player.c
putv_SOURCES+=jitter_common.c
putv_SOURCES+=jitter_sg.c
putv_SOURCES+=jitter_ring.c
putv_LIBS+=pthread
putv_CFLAGS-$(SAMPLERATE_AUTO)+=-DDEFAULT_SAMPLERATE=44100
putv_CFLAGS-$(SAMPLERATE_44100)+=-DDEFAULT_SAMPLERATE=44100
putv_CFLAGS-$(SAMPLERATE_48000)+=-DDEFAULT_SAMPLERATE=48000
putv_SOURCES+=media_common.c
putv_SOURCES+=src_common.c
putv_SOURCES+=decoder_common.c
putv_SOURCES+=sink_common.c

putv_SOURCES+=filter_pcm.c
putv_SOURCES+=filter_rescale.c
putv_SOURCES+=filter_boost.c
putv_SOURCES-$(FILTER_ONECHANNEL)+=filter_mono.c
putv_SOURCES-$(FILTER_MIXED)+=filter_mixed.c
putv_SOURCES-$(FILTER_STATS)+=filter_stats.c
putv_LIBS-$(FILTER_STATS)+=m

putv_SOURCES-$(MEDIA_SQLITE)+=media_sqlite.c
putv_LIBRARY-$(MEDIA_SQLITE)+=sqlite3
putv_LIBRARY-$(MEDIA_SQLITE_EXT)+=jansson
putv_SOURCES-$(MEDIA_FILE)+=media_file.c
putv_SOURCES-$(MEDIA_DIR)+=media_dir.c
putv_SOURCES-$(HEARTBEAT)+=heartbeat_samples.c
putv_SOURCES-$(HEARTBEAT)+=heartbeat_bitrate.c
putv_CFLAGS-$(HEARTBEAT)+=-DHEARTBEAT_COEF_1000=1000
putv_LIBS-$(HEARTBEAT)+=rt
putv_SOURCES-$(SRC_FILE)+=src_file.c
putv_SOURCES-$(SRC_ALSA)+=src_alsa.c
putv_LIBS-$(SRC_ALSA)+=asound
putv_SOURCES-$(SRC_CURL)+=src_curl.c
putv_LIBRARY-$(SRC_CURL)+=libcurl
putv_SOURCES-$(SRC_UNIX)+=src_unix.c
putv_SOURCES-$(SRC_UDP)+=src_udp.c
putv_LIBS-$(SRC_UDP)+=pthread
putv_SOURCES-$(DEMUX_PASSTHROUGH)+=demux_passthrough.c
putv_SOURCES-$(DEMUX_RTP)+=demux_rtp.c
putv_SOURCES-$(DEMUX_DVB)+=demux_dvb.c
putv_SOURCES-$(DECODER_PASSTHROUGH)+=decoder_passthrough.c
ifneq ($(DECODER_MODULES),y)
putv_SOURCES-$(DECODER_MAD)+=decoder_mad.c
putv_LIBRARY-$(DECODER_MAD)+=mad
putv_SOURCES-$(DECODER_FLAC)+=decoder_flac.c
putv_LIBRARY-$(DECODER_FLAC)+=flac
putv_SOURCES-$(DECODER_FAAD2)+=decoder_faad2.c
putv_LIBRARY-$(DECODER_FAAD2)+=faad2
endif
putv_LIBS-$(DECODER_MODULES)+=dl
putv_CFLAGS-$(DECODER_DUMP)+=-DDECODER_DUMP
putv_SOURCES+=encoder_common.c
putv_SOURCES-$(ENCODER_LAME)+=encoder_lame.c
putv_LIBS-$(ENCODER_LAME)+=mp3lame
ifeq ($(ENCODER_FLAC),y)
  ENCODER:=encoder_lame
endif
putv_SOURCES-$(ENCODER_FLAC)+=encoder_flac.c
putv_LIBRARY-$(ENCODER_FLAC)+=flac
ifeq ($(ENCODER_FLAC),y)
  ENCODER:=encoder_flac
endif
putv_SOURCES-$(ENCODER_PASSTHROUGH)+=encoder_passthrough.c
ifeq ($(ENCODER_PASSTHROUGH),y)
  ENCODER:=encoder_passthrough
endif
putv_CFLAGS+=-DENCODER=$(ENCODER)
putv_CFLAGS-$(ENCODER_DUMP)+=-DENCODER_DUMP
putv_SOURCES-$(MUX)+=mux_common.c
putv_SOURCES-$(MUX)+=mux_passthrough.c
putv_SOURCES-$(MUX_RTP)+=mux_rtp.c
putv_SOURCES-$(SINK_ALSA)+=sink_alsa.c
putv_LIBS-$(SINK_ALSA)+=asound
putv_SOURCES-$(SINK_TINYALSA)+=sink_tinyalsa.c
putv_LIBRARY-$(SINK_TINYALSA)+=tinyalsa
putv_SOURCES-$(SINK_FILE)+=sink_file.c
putv_SOURCES-$(SINK_UDP)+=sink_udp.c
putv_SOURCES-$(SINK_UNIX)+=sink_unix.c
putv_SOURCES-$(SINK_UNIX)+=unix_server.c
putv_SOURCES-$(SINK_PULSE)+=sink_pulse.c
putv_LIBRARY-$(SINK_PULSE)+=libpulse-simple
putv_CFLAGS-$(SINK_DUMP)+=-DSINK_DUMP
putv_SOURCES-$(CMDLINE)+=cmds_line.c
putv_SOURCES-$(CMDINPUT)+=cmds_input.c
putv_SOURCES-$(JSONRPC)+=cmds_json.c
putv_SOURCES-$(JSONRPC)+=unix_server.c
putv_LIBRARY-$(JSONRPC)+=jansson
putv_LIBS-$(JSONRPC)+=jsonrpc
putv_CFLAGS-$(JSONRPC)+=-I ../lib/jsonrpc
putv_LDFLAGS-$(JSONRPC)+=-L ../lib/jsonrpc
putv_SOURCES-$(TINYSVCMDNS)+=cmds_tinysvcmdns.c
putv_LIBS-$(TINYSVCMDNS)+=tinysvcmdns
putv_CFLAGS-$(TINYSVCMDNS)+=-I ../lib/tinysvcmdns
putv_LDFLAGS-$(TINYSVCMDNS)+=-L ../lib/tinysvcmdns
putv_LIBRARY-$(USE_ID3TAG)+=id3tag
putv_LIBS-$(USE_OGGMETADDATA)+=FLAC
putv_LIBS-$(USE_TIMER)+=rt

putv_CFLAGS-$(DEBUG)+=-g -DDEBUG -O1

ifeq ($(DECODER_MODULES),y)
modules-$(DECODER_MAD)+=decoder_mad
decoder_mad_SOURCES+=decoder_mad.c
decoder_mad_LIBS+=mad
modules-$(DECODER_FLAC)+=decoder_flac
decoder_flac_CFLAGS-$(SAMPLERATE_AUTO)+=-DDEFAULT_SAMPLERATE=44100
decoder_flac_CFLAGS-$(SAMPLERATE_44100)+=-DDEFAULT_SAMPLERATE=44100
decoder_flac_CFLAGS-$(SAMPLERATE_48000)+=-DDEFAULT_SAMPLERATE=48000
decoder_flac_SOURCES+=decoder_flac.c
decoder_flac_LIBRARY+=FLAC
endif
