TINYSVCMDNS_VERSION = 0.9.0
TINYSVCMDNS_SITE = $(call github,tinysvcmdns,tinysvcmdns,tinysvcmdns-$(TINYSVCMDNS_VERSION))
TINYSVCMDNS_LICENSE = BSD
TINYSVCMDNS_INSTALL_STAGING = YES

define TINYSVCMDNS_BUILD_CMDS
        $(TARGET_CONFIGURE_OPTS) $(TARGET_MAKE_ENV) \
                $(MAKE1) -C $(@D) $(TINYSVCMDNS_MAKE_OPTS)
endef

define TINYSVCMDNS_INSTALL_STAGING_CMDS
	install -D -m 0664 $(@D)/libtinysvcmdns.a $(STAGING_DIR)/usr/lib/libtinysvcmdns.a
	install -D -m 0664 $(@D)/mdnsd.h $(STAGING_DIR)/usr/include/mdnsd.h
endef

$(eval $(generic-package))
