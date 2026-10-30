################################################################################
#
# difiw-shell
#
################################################################################

DIFIW_SHELL_VERSION = 0.4.0
DIFIW_SHELL_SITE = $(BR2_EXTERNAL_DIFIW_PATH)/../src/difiw-shell
DIFIW_SHELL_SITE_METHOD = local
DIFIW_SHELL_LICENSE = MIT
DIFIW_SHELL_LICENSE_FILES = LICENSE

# Static link: no libstdc++ shared library has to be shipped in the rootfs.
define DIFIW_SHELL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) -O2" \
		LDFLAGS="$(TARGET_LDFLAGS) -static"
endef

define DIFIW_SHELL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/difiw-shell $(TARGET_DIR)/usr/bin/difiw-shell
	$(INSTALL) -D -m 0755 $(@D)/build/difiw-gui $(TARGET_DIR)/usr/bin/difiw-gui
endef

$(eval $(generic-package))
