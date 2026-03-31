bin-y+=putv_input
putv_input_SOURCES+=input.c
putv_input_SOURCES+=client_json.c
putv_input_SOURCES+=../src/daemonize.c
putv_input_CFLAGS+=-I../lib/jsonrpc
putv_input_CFLAGS+=-I../src
putv_input_LDFLAGS+=-L$(builddir)/lib/jsonrpc
putv_input_LIBRARY+=jansson
putv_input_LIBS+=jsonrpc pthread
putv_input_LIBS-$(USE_LIBINPUT)+=udev input

putv_input_CFLAGS-$(DEBUG)+=-g -DDEBUG
