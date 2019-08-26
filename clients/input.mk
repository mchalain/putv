bin-y+=putv_input
putv_input_SOURCES+=input.c
putv_input_SOURCES+=client_json.c
putv_input_CFLAGS+=-I../lib/jsonrpc
putv_input_CFLAGS+=-DUSE_INOTIFY
putv_input_LDFLAGS+=-L../lib/jsonrpc
putv_input_LIBS+=jansson pthread
putv_input_LIBRARY+=jsonrpc
putv_input_LIBS-$(USE_LIBINPUT)+=udev input
putv_input_CFLAGS-$(USE_LIBINPUT)+=-DUSE_LIBINPUT
putv_input_CFLAGS-$(JSONRPC_LARGEPACKET)+=-DJSONRPC_LARGEPACKET
putv_input_CFLAGS-$(DEBUG)+=-g -DDEBUG
