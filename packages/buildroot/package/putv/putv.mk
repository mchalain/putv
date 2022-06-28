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

ifeq ($(BR2_PACKAGE_TINYSVCMDNS),y)
PUTV_DEPENDENCIES += tinysvcmdns
endif

PUTV_DATADIR=/srv/www-putv
PUTV_CONFIGURE_OPTS= \
	prefix=/usr \
	datadir=$(PUTV_DATADIR) \
	sysconfdir=/etc/ouistiti

#PUTV_MAKE_OPTS+=V=1
#PUTV_MAKE_OPTS+=DEBUG=y

PUTV_CONFIGURE_OPTS+=UPNPRENDERER=$(BR2_PACKAGE_PUTV_UPNPRENDERER)
PUTV_CONFIGURE_OPTS+=DISPLAY_DIRECTFB=$(BR2_PACKAGE_DIRECTFB)
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

ifeq ($(BR2_PACKAGE_PUTV_UPNPRENDERER),y)
PUTV_DEPENDENCIES += gmrender-resurrect2
endif

ifeq ($(BR2_PACKAGE_DIRECTFB),yy)
PUTV_DEPENDENCIES += directfb
endif

ifeq ($(BR2_PACKAGE_WAVESHARE_EPAPER),y)
PUTV_DEPENDENCIES += waveshare-epaper
endif

ifeq ($(BR2_PACKAGE_PUTV_WEBAPP),y)
define PUTV_WEBAPP_INSTALL_TARGET_CMDS
	ln -sf /media $(TARGET_DIR)$(PUTV_DATADIR)/htdocs/media
endef
else
PUTV_WEBAPP_INSTALL_TARGET_CMDS=
endif

PUTV_MIXER?=Master

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

	$(INSTALL) -D -m 644 $(PUTV_PKGDIR)/putv.in $(@D)/putv.conf
	if [ -n "$(PUTV)" ]; then sed -i "s/%DAEMON%/$(PUTV)/g" $(@D)/putv.conf; fi
	if [ -z "$(PUTV)" ]; then sed -i "s/DAEMON=.*//" $(@D)/putv.conf; fi
	sed -i "s,%PUTV_WEB%,$(PUTV_DATADIR),g" $(@D)/putv.conf
	sed -i "s,%MIXER%,$(PUTV_MIXER),g" $(@D)/putv.conf
	$(INSTALL) -D -m 644 $(@D)/putv.conf \
		$(TARGET_DIR)/etc/default/putv
	$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/putv.sh \
		$(TARGET_DIR)/etc/init.d/putv.sh
	$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/putv_client.sh \
		$(TARGET_DIR)/etc/init.d/putv_client.sh
	if [ "$(BR2_PACKAGE_PUTV_UPNPRENDERER)" == "y" ]; then \
		$(INSTALL) -D -m 755 $(PUTV_PKGDIR)/gmrender.sh \
			$(TARGET_DIR)/etc/init.d/gmrender.sh; \
	fi
	$(call PUTV_WEBAPP_INSTALL_TARGET_CMDS)
endef

define PUTV_INSTALL_INIT_SYSV_GPIOD_CMDS
	$(INSTALL) -D -m 644 $(PUTV_PKGDIR)/gpiod.conf \
		$(TARGET_DIR)/etc/gpiod/rules.d/putv.conf
endef
ifeq ($(BR2_PACKAGE_GPIOD),y)
PUTV_POST_INSTALL_TARGET_HOOKS+=PUTV_INSTALL_INIT_SYSV_GPIOD_CMDS
endif

define PUTV_INSTALL_INIT_SYSV
	ln -sf putv.sh $(TARGET_DIR)/etc/init.d/S30putv
	ln -sf putv_client.sh $(TARGET_DIR)/etc/init.d/S31putv_client

	if [ "$(BR2_PACKAGE_PUTV_UPNPRENDERER)" == "y" ]; then \
		ln -sf gmrender.sh $(TARGET_DIR)/etc/init.d/S80gmrender; \
	fi
endef

$(eval $(generic-package))
