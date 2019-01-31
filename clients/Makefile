bin-y+=display

display_SOURCES+=display.c
display_SOURCES+=client_json.c
display_CFLAGS+=-I../lib/jsonrpc
display_CFLAGS+=-DUSE_INOTIFY
display_LDFLAGS+=-L../lib/jsonrpc
display_LIBS+=jansson
display_LIBRARY+=jsonrpc
display_CFLAGS-$(JSONRPC_LARGEPACKET)+=-DJSONRPC_LARGEPACKET
display_CFLAGS-$(DEBUG)+=-g -DDEBUG

bin-y+=input

input_SOURCES+=input.c
input_SOURCES+=client_json.c
input_CFLAGS+=-I../lib/jsonrpc
input_CFLAGS+=-DUSE_INOTIFY
input_LDFLAGS+=-L../lib/jsonrpc
input_LIBS+=jansson pthread
input_LIBRARY+=jsonrpc
input_LIBS-$(USE_LIBINPUT)+=udev input
input_CFLAGS-$(USE_LIBINPUT)+=-DUSE_LIBINPUT
input_CFLAGS-$(JSONRPC_LARGEPACKET)+=-DJSONRPC_LARGEPACKET
input_CFLAGS-$(DEBUG)+=-g -DDEBUG
