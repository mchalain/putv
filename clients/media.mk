bin-y+=media
media_SOURCES+=media.c
media_SOURCES+=client_json.c
media_CFLAGS+=-I../lib/jsonrpc
media_CFLAGS+=-DUSE_INOTIFY
media_LDFLAGS+=-L../lib/jsonrpc
media_LIBS+=jansson pthread
media_LIBRARY+=jsonrpc
media_CFLAGS-$(JSONRPC_LARGEPACKET)+=-DJSONRPC_LARGEPACKET
media_CFLAGS-$(DEBUG)+=-g -DDEBUG
