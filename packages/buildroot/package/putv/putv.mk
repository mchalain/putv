################################################################################
#
# putv
#
################################################################################

#PUTV_VERSION = 2.0
#PUTV_SOURCE = v$(OUISCLOUD_VERSION).tar.gz
#PUTV_SITE = https://github.com/ouistiti-project/putv/archive
PUTV_VERSION = HEAD
PUTV_SITE = https://github.com/ouistiti-project/putv.git
PUTV_SITE_METHOD = git
PUTV_LICENSE = MIT
PUTV_LICENSE_FILES = LICENSE
PUTV_DEPENDENCIES += libmad flac lame jansson sqlite libid3tag
PUTV_MAKE=$(MAKE1)

ifneq ($(BR2_PACKAGE_PUTV_TINYSVCMDNS),y)
BR2_PACKAGE_PUTV_TINYSVCMDNS=n
endif

ifeq ($(BR2_PACKAGE_TINYSVCMDNS),y)
PUTV_DEPENDENCIES += tinysvcmdns
endif

PUTV?=putv
PUTV_MIXER?=$(BR2_PACKAGE_PUTV_MIXER)
PUTV_FILTER?="$(BR2_PACKAGE_PUTV_FILTER)"
PUTV_DATADIR?=/srv/www-putv
ifeq ($(BR2_PACKAGE_PUTV_STREAM_SERVER),y)
  PUTV_SERVER_OUTPUT="rtp://239.255.0.1:4400"
  PUTV_WSNAME_SINK="putv_sink"
else
  PUTV_WSNAME_SINK="putv"
endif


PUTV_CONFIGURE_OPTS= \
	prefix=/usr \
	datadir=$(PUTV_DATADIR) \
	sysconfdir=/etc/ouistiti

#PUTV_MAKE_OPTS+=V=1
#PUTV_MAKE_OPTS+=DEBUG=y

ifeq ($(BR2_PACKAGE_TINYSVCMNDS),y)
 PUTV_CONFIGURE_OPTS+=TINYSVCMDNS=y
else
 PUTV_CONFIGURE_OPTS+=TINYSVCMDNS=$(BR2_PACKAGE_PUTV_TINYSVCMDNS)
 PUTV_CONFIGURE_OPTS+=TINYSVCMDNS_INTERN=y
endif
PUTV_CONFIGURE_OPTS+=UPNPRENDERER=$(BR2_PACKAGE_PUTV_UPNPRENDERER)
ifeq ($(BR2_PACKAGE_DIRECTFB),y)
PUTV_CONFIGURE_OPTS+=DISPLAY_DIRECTFB=y
endif
PUTV_CONFIGURE_OPTS+=DISPLAY_EPAPER=$(BR2_PACKAGE_WAVESHARE_EPAPER)
PUTV_CONFIGURE_OPTS+=JSONRPC=$(BR2_PACKAGE_JANSSON)
PUTV_CONFIGURE_OPTS+=DECODER_FAAD2=$(BR2_PACKAGE_FAAD2)
PUTV_CONFIGURE_OPTS+=DECODER_LAME=$(BR2_PACKAGE_LAME)
PUTV_CONFIGURE_OPTS+=DECODER_FLAC=$(BR2_PACKAGE_FLAC)
PUTV_CONFIGURE_OPTS+=ENCODER_FLAC=$(BR2_PACKAGE_FLAC)
PUTV_CONFIGURE_OPTS+=ENCODER_MAD=$(BR2_PACKAGE_MAD)
PUTV_CONFIGURE_OPTS+=SRC_CURL=$(BR2_PACKAGE_LIBCURL)
PUTV_CONFIGURE_OPTS+=SRC_ALSA=$(BR2_PACKAGE_ALSA_LIB)
PUTV_CONFIGURE_OPTS+=SINK_ALSA=$(BR2_PACKAGE_ALSA_LIB)
PUTV_CONFIGURE_OPTS+=SINK_TINYALSA=$(BR2_PACKAGE_TINYALSA)
PUTV_CONFIGURE_OPTS+=WEBAPP=$(BR2_PACKAGE_PUTV_WEBAPP)
PUTV_CONFIGURE_OPTS+=CMDLINE_DOWNLOAD=y
PUTV_CONFIGURE_OPTS+=UDP_STATISTIC=y

ifeq ($(BR2_PACKAGE_PUTV_UPNPRENDERER),y)
PUTV_DEPENDENCIES += gmrender-resurrect2
endif

ifeq ($(BR2_PACKAGE_DIRECTFB),yy)
PUTV_DEPENDENCIES += directfb
endif

ifeq ($(BR2_PACKAGE_WAVESHARE_EPAPER),y)
PUTV_DEPENDENCIES += waveshare-epaper
endif

define PUTV_INSTALL_CONFIG
	$(Q)cat $(PUTV_PKGDIR)/putv.in \
		| sed "s,%PUTV_WEB%,$(PUTV_DATADIR),g" \
		| sed "s,%FILTER%,$1,g" \
		| sed "s,%MIXER%,$2,g" \
		| sed "s,%OUTPUT%,$3,g" \
		| sed "s,%WEBSOCKETNAME%,$4,g" \
		> $5
	$(call PUTV_ROTARY_VOLUME_INSTALL_TARGET_CMDS,$5)
	$(INSTALL) -D -m 644 $5 $(TARGET_DIR)/etc/default/$6
endef
ifeq ($(BR2_PACKAGE_PUTV_STREAM_SERVER),y)
PUTV_INSTALL_CONFIG_SERVER=$(call PUTV_INSTALL_CONFIG,"pcm?stereo",,$(PUTV_SERVER_OUTPUT),putv,$(@D)/putv_server.conf,S30putv_server)
PUTV_INSTALL_SERVER_SYSV=ln -sf putv.sh $(TARGET_DIR)/etc/init.d/S30putv_server
endif

ifeq ($(BR2_PACKAGE_PUTV_WEBAPP),y)
define PUTV_WEBAPP_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)$(PUTV_DATADIR)/htdocs/media
	ln -sf /media $(TARGET_DIR)$(PUTV_DATADIR)/htdocs/media
	ln -sf /tmp/run/websocket $(TARGET_DIR)$(PUTV_DATADIR)/htdocs/websocket
endef
else
PUTV_WEBAPP_INSTALL_TARGET_CMDS=
endif

ifeq ($(BR2_PACKAGE_MEDIAKEYS_ROTARY),y)
define PUTV_ROTARY_VOLUME_INSTALL_TARGET_CMDS
	echo "OPTIONS_CINPUT=\"-i /dev/input/event1\"" >> $1
endef
endif

ifeq ($(BR2_PACKAGE_GPIOD)$(BR2_PACKAGE_PUTV_GPIOD),yy)
define PUTV_GPIOD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 644 $(PUTV_PKGDIR)/gpiod.conf \
		$(TARGET_DIR)/etc/gpiod/rules.d/putv.conf
endef
endif


define PUTV_CONFIGURE_CMDS
	$(TARGET_MAKE_ENV) $(MAKE1) -C $(@D) $(TARGET_CONFIGURE_OPTS) $(PUTV_CONFIGURE_OPTS) defconfig
endef

define PUTV_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE1) -C $(@D) $(TARGET_CONFIGURE_OPTS) $(PUTV_MAKE_OPTS)
endef

define PUTV_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE1) -C $(@D) $(TARGET_CONFIGURE_OPTS) $(PUTV_MAKE_OPTS) DESTDIR=$(TARGET_DIR) install
	mkdir -p $(TARGET_DIR)$(PUTV_DATADIR)/htdocs/apps/
	$(INSTALL) -D -m 644 $(PUTV_PKGDIR)/ouiradio.json \
		$(TARGET_DIR)$(PUTV_DATADIR)/htdocs/apps/ouiradio.json

	$(call PUTV_INSTALL_CONFIG,$(PUTV_FILTER),$(PUTV_MIXER),,$(PUTV_WSNAME_SINK),$(@D)/putv.conf,putv)
	$(call PUTV_INSTALL_CONFIG_SERVER)

	$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/putv.sh \
		$(TARGET_DIR)/etc/init.d/putv.sh
	$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/putv_client.sh \
		$(TARGET_DIR)/etc/init.d/putv_client.sh
	$(INSTALL) -D -m 644 $(PUTV_PKGDIR)/putv.cmdline \
		$(TARGET_DIR)/etc/default/putv.cmdline
	if [ "$(BR2_PACKAGE_PUTV_UPNPRENDERER)" == "y" ]; then \
		$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/gmrender.sh \
			$(TARGET_DIR)/etc/init.d/gmrender.sh; \
	fi
	$(call PUTV_WEBAPP_INSTALL_TARGET_CMDS)
	$(call PUTV_GPIOD_INSTALL_TARGET_CMDS)
endef

define PUTV_INSTALL_INIT_SYSV
	$(call PUTV_INSTALL_SERVER_SYSV)
	ln -sf putv.sh $(TARGET_DIR)/etc/init.d/S30putv
	ln -sf putv_client.sh $(TARGET_DIR)/etc/init.d/S31putv_client

	if [ "$(BR2_PACKAGE_PUTV_UPNPRENDERER)" == "y" ]; then \
		ln -sf gmrender.sh $(TARGET_DIR)/etc/init.d/S80gmrender; \
	fi
endef

$(eval $(generic-package))
