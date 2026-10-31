################################################################################
#
# difiw-desktop
#
################################################################################

DIFIW_DESKTOP_VERSION = 0.5.6
DIFIW_DESKTOP_SITE = $(BR2_EXTERNAL_DIFIW_PATH)/../src/difiw-desktop
DIFIW_DESKTOP_SITE_METHOD = local
DIFIW_DESKTOP_LICENSE = MIT
DIFIW_DESKTOP_LICENSE_FILES = LICENSE

# Static link: nothing but the kernel is needed at run time.
define DIFIW_DESKTOP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -O2" \
		LDFLAGS="$(TARGET_LDFLAGS) -static"
endef

define DIFIW_DESKTOP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/difiw-desktop $(TARGET_DIR)/usr/bin/difiw-desktop
	$(INSTALL) -D -m 0755 $(@D)/build/difiw-apps $(TARGET_DIR)/usr/bin/difiw-apps
	$(INSTALL) -D -m 0755 $(@D)/build/difiwctl $(TARGET_DIR)/usr/bin/difiwctl
endef

$(eval $(generic-package))
