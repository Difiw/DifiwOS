# Difiw desktop build. Used by Buildroot (cross) and for host development.
# SPDX-License-Identifier: MIT
CXX      ?= g++
CXXFLAGS ?= -O2
STD       = -std=c++17
WARN      = -Wall -Wextra -Wpedantic -Wno-unused-parameter
LDFLAGS  ?=
BUILD     = build

COMMON = gfx.cpp
DESK   = desktop_core.cpp desktop_input.cpp desktop_ui.cpp desktop_panels.cpp desktop_main.cpp tasks.cpp $(COMMON)
APPS   = apps.cpp client.cpp $(COMMON)
HDRS   = $(wildcard *.hpp)

all: $(BUILD)/difiw-desktop $(BUILD)/difiw-apps $(BUILD)/difiwctl

$(BUILD)/difiw-desktop: $(DESK) $(HDRS)
	mkdir -p $(BUILD)
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) -o $@ $(DESK) $(LDFLAGS)

$(BUILD)/difiw-apps: $(APPS) $(HDRS)
	mkdir -p $(BUILD)
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) -o $@ $(APPS) $(LDFLAGS)

$(BUILD)/difiwctl: difiwctl.cpp proto.hpp
	mkdir -p $(BUILD)
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) -o $@ difiwctl.cpp $(LDFLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
