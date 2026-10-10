UVCSHOW_VERSION = 1.0
UVCSHOW_SITE = $(BR2_EXTERNAL_LICHEEPI_NANO_PATH)/package/uvcshow
UVCSHOW_SITE_METHOD = local
UVCSHOW_LICENSE = MIT
UVCSHOW_LICENSE_FILES = uvcshow.c
UVCSHOW_DEPENDENCIES = jpeg

ifeq ($(BR2_PACKAGE_UVCSHOW_SCALE_FULL),y)
UVCSHOW_OPTS = -s 1
else
UVCSHOW_OPTS = -s 2
endif

define UVCSHOW_BUILD_CMDS
	(cd $(@D) && $(TARGET_CONFIGURE_OPTS) $(TARGET_CC) \
		$(TARGET_CFLAGS) -O2 -Wall $(TARGET_LDFLAGS) \
		-o uvcshow uvcshow.c -ljpeg)
endef

define UVCSHOW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uvcshow $(TARGET_DIR)/usr/bin/uvcshow
	$(INSTALL) -D -m 0755 $(UVCSHOW_PKGDIR)/S99uvcshow $(TARGET_DIR)/etc/init.d/S99uvcshow
endef

define UVCSHOW_WRITE_OPTS
	printf '%s\n' "$(UVCSHOW_OPTS)" > $(TARGET_DIR)/etc/uvcshow.opts
endef
UVCSHOW_POST_INSTALL_TARGET_HOOKS += UVCSHOW_WRITE_OPTS

$(eval $(generic-package))
