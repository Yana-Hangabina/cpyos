cmake_minimum_required(VERSION 3.16)

# CPYOS Metalio E-INK4 Plus bring-up target.
# This is an ESP-IDF 6.1 project, not a PlatformIO/Arduino target: ESP32-S31 is
# not supported by the Arduino core used by the Read Pico build.
# The local test script stamps bring-up images with -DPROJECT_VER=...-untested;
# normal builds keep the product version.
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.3.1-plus-test1")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cpyos_metalio_plus)
