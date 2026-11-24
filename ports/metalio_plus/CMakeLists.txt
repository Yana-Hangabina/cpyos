cmake_minimum_required(VERSION 3.16)

# CPYOS Metalio E-INK4 Plus bring-up target.
# This is an ESP-IDF 6.1 project, not a PlatformIO/Arduino target: ESP32-S31 is
# not supported by the Arduino core used by the Read Pico build.
# The local test script stamps bring-up images with -DPROJECT_VER=...-untested;
# normal builds keep the product version.
if(NOT DEFINED PROJECT_VER)
    # AGENTS §66.4: every device image carries its own tag so a version string
    # identifies the hardware without the About/crash pages lying.
    set(PROJECT_VER "0.8.0-e4p")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cpyos_metalio_plus)

# Target isolation (docs/engineering/target-isolation.md): this tree only
# builds the RISC-V ESP32-S31. A stale sdkconfig from a readpico/S3 checkout
# would otherwise configure a different chip and silently produce an image the
# Plus cannot boot.
if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR
        "Metalio E-INK4 Plus must be built for esp32s31 (current IDF_TARGET=${IDF_TARGET}). "
        "Run: idf.py --preview set-target esp32s31, or keep readpico builds in their own tree.")
endif()
