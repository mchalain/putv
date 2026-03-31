
bin-y+=putv_display
putv_display_SOURCES+=display.c
putv_display_SOURCES+=crc32.c
putv_display_SOURCES+=display_console.c
putv_display_SOURCES+=client_json.c
putv_display_CFLAGS+=-I../lib/jsonrpc
putv_display_LDFLAGS+=-L$(builddir)/lib/jsonrpc
putv_display_LIBRARY+=jansson
putv_display_LIBS+=jsonrpc

putv_display_SOURCES-$(DISPLAY_DIRECTFB)+=display_directfb.c
putv_display_CFLAGS-$(DISPLAY_DIRECTFB)+=-I=/usr/include/directfb
putv_display_LIBS-$(DISPLAY_DIRECTFB)+=directfb pthread

putv_display_CFLAGS-$(DEBUG)+=-g -DDEBUG
