# Netline 0.5.6 - browser for Difiw OS on top of the Chromium Embedded Framework (CEF).
# Chrome (tabs, address bar, menu) is drawn with Difiw's own gfx; web content is rendered by CEF off-screen.
#   Difiw OS:   cmake -S . -B build -DNETLINE_BACKEND=difiw -DCEF_ROOT=<unpacked CEF>   (Buildroot package difiw-netline)
#   dev / test: cmake -S . -B build -DNETLINE_BACKEND=sdl   -DCEF_ROOT=<unpacked CEF>   (normal Linux desktop, needs SDL2)
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.19)
project(netline C CXX)
set(NETLINE_BACKEND "sdl" CACHE STRING "difiw or sdl")
set(CEF_ROOT "$ENV{HOME}/netline-cef/cef" CACHE PATH "unpacked CEF binary distribution")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

set(SRC netline.cpp ../difiw-desktop/gfx.cpp)
if(NETLINE_BACKEND STREQUAL "sdl")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  list(APPEND SRC platform_sdl.cpp)
else()
  list(APPEND SRC platform_difiw.cpp ../difiw-desktop/client.cpp)
endif()

add_executable(netline ${SRC})
set_target_properties(netline PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                      BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
target_include_directories(netline PRIVATE ${CEF_ROOT})
target_compile_options(netline PRIVATE -Wall -Wno-unused-parameter -fno-exceptions)
target_link_libraries(netline libcef_dll_wrapper ${CEF_ROOT}/Release/libcef.so)
if(NETLINE_BACKEND STREQUAL "sdl")
  target_include_directories(netline PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(netline ${SDL2_LIBRARIES})
endif()

# CEF runtime files next to the executable (Buildroot package installs this directory to /opt/netline)
add_custom_command(TARGET netline POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CEF_ROOT}/Release $<TARGET_FILE_DIR:netline>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CEF_ROOT}/Resources $<TARGET_FILE_DIR:netline>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/res $<TARGET_FILE_DIR:netline>/res)
