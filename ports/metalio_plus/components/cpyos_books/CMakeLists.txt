if(NOT DEFINED ENV{CPYOS_UI_PROBE} AND NOT DEFINED ENV{CPYOS_READER_DEMO})
    # Only the bring-up/probe and reader-demo images link the book engine today.
    idf_component_register()
    return()
endif()

# CPYOS book engine: ZIP/miniz, EPUB (OPF/nav/XHTML/CSS/hyphenation/blocks),
# TXT, FB2, XTC, CBZ and the library index. Sources are the shared repository
# files; this component only supplies the build wiring for IDF.
set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
file(GLOB BOOK_SRCS
    "${REPO_ROOT}/lib/ZipFile/*.cpp"
    "${REPO_ROOT}/lib/miniz/src/*.cpp"
    "${REPO_ROOT}/lib/miniz/src/*.c"
    "${REPO_ROOT}/lib/Epub/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/blocks/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/converters/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/css/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/parsers/*.cpp"
    "${REPO_ROOT}/lib/Epub/Epub/hyphenation/*.cpp"
    "${REPO_ROOT}/lib/Txt/*.cpp"
    "${REPO_ROOT}/lib/Fb2/*.cpp"
    "${REPO_ROOT}/lib/Xtc/*.cpp"
    "${REPO_ROOT}/lib/Xtc/Xtc/*.cpp"
    "${REPO_ROOT}/lib/Cbz/*.cpp"
    "${REPO_ROOT}/lib/LibraryIndex/*.cpp"
    "${REPO_ROOT}/lib/DeviceSecret/*.cpp"
    "${REPO_ROOT}/lib/JpegToBmpConverter/*.cpp"
    "${REPO_ROOT}/lib/PngToBmpConverter/*.cpp"
    "${REPO_ROOT}/lib/GifToBmpConverter/*.cpp"
    "${REPO_ROOT}/lib/TrustedTime/*.cpp"
    "${REPO_ROOT}/lib/FsHelpers/*.cpp"
    "${REPO_ROOT}/src/util/StringUtils.cpp"
    "${REPO_ROOT}/lib/WeReadWebApi/src/WeReadStore.cpp"
    "${REPO_ROOT}/lib/WeReadWebApi/src/WeReadProtocol.cpp"
    "${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/src/ProtectedBook.cpp"
    "${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/src/Zip.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/content_crypto_psa.cpp")

# The shared glue selects a platform Crypto backend with
# FREEINK_CONTENT_PORT_PSA; this port supplies AES-CBC + hashes through PSA and
# refuses the legacy RSA/PKCS#12 helpers.

idf_component_register(
    SRCS ${BOOK_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../cpyos_ui/compat"
        "${REPO_ROOT}/lib/Epub"
        "${REPO_ROOT}/lib/Epub/Epub"
        "${REPO_ROOT}/lib/Epub/Epub/blocks"
        "${REPO_ROOT}/lib/Epub/Epub/converters"
        "${REPO_ROOT}/lib/Epub/Epub/css"
        "${REPO_ROOT}/lib/Epub/Epub/parsers"
        "${REPO_ROOT}/lib/Epub/Epub/hyphenation"
        "${REPO_ROOT}/lib/ZipFile"
        "${REPO_ROOT}/lib/miniz/src"
        "${REPO_ROOT}/lib/InflateReader"
        "${REPO_ROOT}/lib/Txt"
        "${REPO_ROOT}/lib/Fb2"
        "${REPO_ROOT}/lib/Xtc"
        "${REPO_ROOT}/lib/Xtc/Xtc"
        "${REPO_ROOT}/lib/Cbz"
        "${REPO_ROOT}/lib/LibraryIndex"
        "${REPO_ROOT}/lib/XmlParserUtils"
        "${REPO_ROOT}/lib/DeviceSecret"
        "${REPO_ROOT}/lib/GifSupport"
        "${REPO_ROOT}/lib/TrustedTime"
        "${REPO_ROOT}/lib/WeReadWebApi/src"
        "${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include"
        "${REPO_ROOT}/lib/JpegToBmpConverter"
        "${REPO_ROOT}/lib/PngToBmpConverter"
        "${REPO_ROOT}/lib/GifToBmpConverter"
        "${REPO_ROOT}/lib/uzlib/src"
        "${REPO_ROOT}/lib/expat"
        "${REPO_ROOT}/lib/GfxRenderer"
        "${REPO_ROOT}/lib/EpdFont"
        "${REPO_ROOT}/lib/Memory"
        "${REPO_ROOT}/lib/Logging"
        "${REPO_ROOT}/lib/Utf8"
        "${REPO_ROOT}/lib/Serialization"
        "${REPO_ROOT}/lib/FsHelpers"
        "${REPO_ROOT}/lib/MiniBidi"
        "${REPO_ROOT}/lib/hal"
        "${REPO_ROOT}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../cpyos_ui/compat"
        "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include"
        "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include"
        "${CMAKE_CURRENT_LIST_DIR}/private_include"
    REQUIRES cpyos_ui expat_lib uzlib_lib imgcodecs esp_psram nvs_flash esp_timer esp_hw_support esp_mm esp_http_client mbedtls)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # The shared logging macros compile to nothing without these; that silently
    # hid every failure inside JpegToBmpConverter (a conversion could return
    # false with no line in the log at all).
    ENABLE_SERIAL_LOG
    LOG_LEVEL=2
    ARDUINO=10800
    FREEINK_DEVICE_READPICO=1
    FREEINK_CONTENT_PORT_PSA=1
    CONTENT_EXTERNAL_MINIZ=1
    BOARD_HAS_PSRAM=1
    CROSSPOINT_VECTOR_FONTS=1
    ENABLE_CHINESE_VERSION=1
    USE_UTF8_LONG_NAMES=1
    DESTRUCTOR_CLOSES_FILE=1
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES=1
    XML_GE=0
    XML_CONTEXT_BYTES=0
    PNG_MAX_BUFFERED_PIXELS=16416
    MAX_WIDTH=3072)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include Arduino.h>
    -Wno-error -Wno-narrowing -Wno-format -Wno-unused-parameter -Wno-sign-compare
    -Wno-error=maybe-uninitialized -Wno-error=array-bounds -Wno-error=comment
    -Wno-error=missing-field-initializers)
