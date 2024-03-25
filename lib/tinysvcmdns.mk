lib-y+=tinysvcmdns

tinysvcmdns_SOURCES+=tinysvcmdns/mdns.c
tinysvcmdns_SOURCES+=tinysvcmdns/mdnsd.c
tinysvcmdns_CFLAGS+=-DNDEBUG
tinysvcmdns_LIBS+=pthread
