# Difiw Shell build. Used by Buildroot (cross) and by `make test` (host).
# SPDX-License-Identifier: MIT
CXX      ?= g++
CXXFLAGS ?= -O2
STD       = -std=c++17
WARN      = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
LDFLAGS  ?=

SRC = main.cpp commands.cpp parser.cpp format.cpp
BUILD = build

all: $(BUILD)/difiw-shell

$(BUILD)/difiw-shell: $(SRC) commands.hpp parser.hpp format.hpp
	mkdir -p $(BUILD)
	$(CXX) $(STD) $(WARN) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Host unit tests (parser + formatting).
$(BUILD)/test_core: ../../tests/test_core.cpp parser.cpp format.cpp parser.hpp format.hpp
	mkdir -p $(BUILD)
	g++ $(STD) $(WARN) -g -fsanitize=address,undefined -I. -o $@ ../../tests/test_core.cpp parser.cpp format.cpp

test: $(BUILD)/test_core
	$(BUILD)/test_core

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
