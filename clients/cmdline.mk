bin-y+=putv_cmdline
putv_cmdline_SOURCES+=cmdline.c
putv_cmdline_SOURCES+=../src/daemonize.c
putv_cmdline_SOURCES+=client_json.c
putv_cmdline_CFLAGS+=-I../src/
putv_cmdline_CFLAGS+=-I../lib/jsonrpc
putv_cmdline_LDFLAGS+=-L../lib/jsonrpc
putv_cmdline_LIBS+=jsonrpc pthread
putv_cmdline_LIBRARY+=jansson
putv_cmdline_LIBS-$(CMDLINE_DOWNLOAD)+=curl

putv_cmdline_CFLAGS-$(DEBUG)+=-g -DDEBUG
