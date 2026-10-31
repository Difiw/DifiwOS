################################################################################
#
# difiw-netline
#
################################################################################

DIFIW_NETLINE_VERSION = 0.5.6
DIFIW_NETLINE_SITE = $(BR2_EXTERNAL_DIFIW_PATH)/../src
DIFIW_NETLINE_SITE_METHOD = local
DIFIW_NETLINE_LICENSE = MIT (Netline), BSD-3-Clause (CEF), Chromium third-party licenses (/opt/netline/CREDITS.html)
DIFIW_NETLINE_LICENSE_FILES = netline-cef/LICENSE
DIFIW_NETLINE_DEPENDENCIES = host-cmake libnss libglib2 dbus at-spi2-core cups libxkbcommon cairo pango fontconfig alsa-lib libudev-zero mesa3d dejavu

# The official CEF binary distribution is fetched and SHA-1 checked by scripts/fetch-cef.sh (run by scripts/build.sh).
DIFIW_NETLINE_CEF_TARBALL = $(BR2_DL_DIR)/difiw-netline/cef_binary_linux64_minimal.tar.bz2

define DIFIW_NETLINE_BUILD_CMDS
	test -s $(DIFIW_NETLINE_CEF_TARBALL) || { echo "missing $(DIFIW_NETLINE_CEF_TARBALL): run scripts/fetch-cef.sh"; exit 1; }
	rm -rf $(@D)/cef && mkdir -p $(@D)/cef && tar -xjf $(DIFIW_NETLINE_CEF_TARBALL) -C $(@D)/cef --strip-components=1
	$(HOST_DIR)/bin/cmake -S $(@D)/netline-cef -B $(@D)/build-netline -G "Unix Makefiles" \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_BUILD_TYPE=Release -DNETLINE_BACKEND=difiw -DCEF_ROOT=$(@D)/cef
	$(HOST_DIR)/bin/cmake --build $(@D)/build-netline -j$(PARALLEL_JOBS)
endef

DIFIW_NETLINE_OPT = $(TARGET_DIR)/opt/netline
define DIFIW_NETLINE_INSTALL_TARGET_CMDS
	rm -rf $(DIFIW_NETLINE_OPT) && mkdir -p $(DIFIW_NETLINE_OPT)/locales $(DIFIW_NETLINE_OPT)/res
	$(INSTALL) -m 0755 $(@D)/build-netline/netline $(DIFIW_NETLINE_OPT)/netline
	$(INSTALL) -m 0755 $(@D)/cef/Release/libcef.so $(DIFIW_NETLINE_OPT)/libcef.so
	$(TARGET_STRIP) --strip-unneeded $(DIFIW_NETLINE_OPT)/libcef.so
	$(INSTALL) -m 0755 $(@D)/cef/Release/libvk_swiftshader.so $(DIFIW_NETLINE_OPT)/
	$(INSTALL) -m 0755 $(@D)/cef/Release/libvulkan.so.1 $(DIFIW_NETLINE_OPT)/
	$(INSTALL) -m 0644 $(@D)/cef/Release/vk_swiftshader_icd.json $(@D)/cef/Release/v8_context_snapshot.bin $(DIFIW_NETLINE_OPT)/
	$(INSTALL) -m 0755 $(@D)/cef/Release/chrome-sandbox $(DIFIW_NETLINE_OPT)/chrome-sandbox
	$(INSTALL) -m 0644 $(@D)/cef/Resources/icudtl.dat $(@D)/cef/Resources/resources.pak \
		$(@D)/cef/Resources/chrome_100_percent.pak $(@D)/cef/Resources/chrome_200_percent.pak $(DIFIW_NETLINE_OPT)/
	$(INSTALL) -m 0644 $(@D)/cef/Resources/locales/ru.pak $(@D)/cef/Resources/locales/en-US.pak $(DIFIW_NETLINE_OPT)/locales/
	$(INSTALL) -m 0644 $(@D)/cef/LICENSE.txt $(@D)/cef/CREDITS.html $(DIFIW_NETLINE_OPT)/
	$(INSTALL) -m 0644 $(@D)/netline-cef/res/netline.dicon $(@D)/netline-cef/res/netline.png $(@D)/netline-cef/res/newtab.html $(DIFIW_NETLINE_OPT)/res/
	$(INSTALL) -D -m 0755 $(@D)/netline-cef/difiw-netline.sh $(TARGET_DIR)/usr/libexec/difiw-netline
endef

$(eval $(generic-package))
