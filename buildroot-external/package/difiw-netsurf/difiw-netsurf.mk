################################################################################
#
# difiw-netsurf
#
################################################################################

DIFIW_NETSURF_VERSION = 3.11
DIFIW_NETSURF_SITE = $(BR2_EXTERNAL_DIFIW_PATH)/../src/netsurf
DIFIW_NETSURF_SITE_METHOD = local
DIFIW_NETSURF_LICENSE = GPL-2.0 (NetSurf), MIT (libraries)
DIFIW_NETSURF_DEPENDENCIES = host-pkgconf libcurl libpng jpeg freetype expat dejavu openssl zlib

# The script downloads the pinned netsurf-all tarball (cached in dl/), patches libnsfb/fbtk and cross-builds
# all NetSurf libraries and the framebuffer front end against this Buildroot sysroot.
define DIFIW_NETSURF_BUILD_CMDS
	BR_OUT=$(BASE_DIR) DIFIW_BUILD_ROOT=$(dir $(BASE_DIR)) \
		$(BR2_EXTERNAL_DIFIW_PATH)/../scripts/netsurf-build.sh $(@D)/dest
endef

define DIFIW_NETSURF_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dest/usr/bin/netsurf-fb $(TARGET_DIR)/usr/bin/netsurf-fb
	mkdir -p $(TARGET_DIR)/usr/share/netsurf
	cp -r $(@D)/dest/usr/share/netsurf/. $(TARGET_DIR)/usr/share/netsurf/
endef

$(eval $(generic-package))
