if(NOT DEFINED ENV{CPYOS_UI_PROBE} AND NOT DEFINED ENV{CPYOS_READER_DEMO})
    # Only the bring-up/probe and reader-demo images link the book engine today.
    idf_component_register()
    return()
endif()

# bitbank2 JPEGDEC / PNGdec / AnimatedGIF, vendored from the CPYOS PlatformIO
# dependency tree (Apache-2.0; see each subdirectory's LICENSE). The book
# engine's image converters need all three, and the Plus port builds offline.
file(GLOB CODEC_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/JPEGDEC/src/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/JPEGDEC/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/PNGdec/src/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PNGdec/src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/AnimatedGIF/src/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AnimatedGIF/src/*.c")

idf_component_register(
    SRCS ${CODEC_SRCS}
    INCLUDE_DIRS
        "JPEGDEC/src"
        "PNGdec/src"
        "AnimatedGIF/src"
        "${CMAKE_CURRENT_LIST_DIR}/../cpyos_ui/compat"
    REQUIRES heap log esp_psram esp_timer)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ARDUINO=10800)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include Arduino.h>
    -Wno-error -Wno-format -Wno-unused-parameter -Wno-sign-compare)
