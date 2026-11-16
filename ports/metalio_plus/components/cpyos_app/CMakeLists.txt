# CPYOS activity shell on the Plus port.
#
# This component compiles the shared activity layer with the platform gaps
# guarded (web-server activity disabled by CROSSPOINT_PORT_NO_NETWORK_SHELL).
# It is opt-in via CPYOS_ACTIVITY_PROBE until the full shell is validated.
if(NOT DEFINED ENV{CPYOS_ACTIVITY_PROBE})
    idf_component_register()
    return()
endif()

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB AVATAR_SRCS "${REPO_ROOT}/lib/AvatarGen/*.cpp")
file(GLOB WEBBROWSER_SRCS "${REPO_ROOT}/lib/WebBrowser/src/*.cpp")
file(GLOB OPDS_SRCS "${REPO_ROOT}/lib/OpdsParser/*.cpp")
file(GLOB CATALOG_SRCS "${REPO_ROOT}/freeink-sdk/libs/network/CatalogList/src/*.cpp")
file(GLOB_RECURSE ACTIVITY_SRCS "${REPO_ROOT}/src/activities/*.cpp")
file(GLOB_RECURSE COMPONENT_SRCS "${REPO_ROOT}/src/components/*.cpp")
file(GLOB_RECURSE THEME_SRCS "${REPO_ROOT}/src/components/themes/*.cpp")
file(GLOB_RECURSE UTIL_SRCS "${REPO_ROOT}/src/util/*.cpp")
file(GLOB_RECURSE NET_SRCS "${REPO_ROOT}/src/network/*.cpp")
file(GLOB APP_SRCS "${REPO_ROOT}/src/*.cpp")
file(GLOB KOREADER_SRCS "${REPO_ROOT}/lib/KOReaderSync/*.cpp")
file(GLOB WEREAD_SRCS "${REPO_ROOT}/lib/WeReadWebApi/src/*.cpp")
file(GLOB PINYIN_SRCS "${REPO_ROOT}/lib/Pinyin/*.cpp")
file(GLOB DEVICE_SECRET_SRCS "${REPO_ROOT}/lib/DeviceSecret/*.cpp")

# Already compiled by cpyos_ui/cpyos_books, or not portable yet.
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/network/CrossPointWebServerActivity\\.cpp$")
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/network/CalibreConnectActivity\\.cpp$")
list(FILTER THEME_SRCS EXCLUDE REGEX "/UITheme\\.cpp$")
list(FILTER THEME_SRCS EXCLUDE REGEX "/BaseTheme\\.cpp$")
list(FILTER THEME_SRCS EXCLUDE REGEX "/CpyTheme\\.cpp$")
list(FILTER THEME_SRCS EXCLUDE REGEX "/UpstreamStyleTheme\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/main\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/CrossPointSettings\\.cpp$")
list(FILTER APP_SRCS EXCLUDE REGEX "/RecentBooksStore\\.cpp$")
list(FILTER NET_SRCS EXCLUDE REGEX "/CrossPointWebServer\\.cpp$")
list(FILTER NET_SRCS EXCLUDE REGEX "/WebDAVHandler\\.cpp$")
list(FILTER WEREAD_SRCS EXCLUDE REGEX "/WeReadStore\\.cpp$")
list(FILTER WEREAD_SRCS EXCLUDE REGEX "/WeReadProtocol\\.cpp$")

file(GLOB UI_SRCS "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/*.cpp")

set(APP_SOURCES
    "bluetooth_activity_stub.cpp"
    "plus_bt_audio_activity.cpp"
    "plus_camera_activity.cpp"
    "plus_transfer_activity.cpp"
    "plus_transfer_hooks.cpp"
    "plus_ota.cpp"
    "plus_ota_activity.cpp"
    "plus_voice_activity.cpp"
    "plus_voice_client.cpp"
    "plus_voice_memo_activity.cpp"
    "power_off_activity.cpp"
    ${ACTIVITY_SRCS}
    ${COMPONENT_SRCS}
    ${THEME_SRCS}
    ${UTIL_SRCS}
    ${NET_SRCS}
    ${APP_SRCS}
    ${KOREADER_SRCS}
    ${WEREAD_SRCS}
    ${PINYIN_SRCS}
    ${DEVICE_SECRET_SRCS}
    ${AVATAR_SRCS}
    ${WEBBROWSER_SRCS}
    ${OPDS_SRCS}
    ${CATALOG_SRCS})

set(APP_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../cpyos_ui/compat"
    "${REPO_ROOT}/src"
    "${REPO_ROOT}/src/activities"
    "${REPO_ROOT}/src/components"
    "${REPO_ROOT}/lib/AvatarGen"
    "${REPO_ROOT}/lib/Epub"
    "${REPO_ROOT}/lib/Epub/Epub"
    "${REPO_ROOT}/lib/EpdFont"
    "${REPO_ROOT}/lib/GfxRenderer"
    "${REPO_ROOT}/lib/LibraryIndex"
    "${REPO_ROOT}/lib/Memory"
    "${REPO_ROOT}/lib/Serialization"
    "${REPO_ROOT}/lib/hal"
    "${REPO_ROOT}/lib/OpdsParser"
    "${REPO_ROOT}/lib/Pinyin"
    "${REPO_ROOT}/lib/DeviceSecret"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/Imu/include"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/Rtc/include"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/FrontlightManager/include"
    "${REPO_ROOT}/lib/KOReaderSync"
    "${REPO_ROOT}/lib/WebBrowser/src"
    "${REPO_ROOT}/lib/WeReadWebApi/src"
    "${REPO_ROOT}/lib/XmlParserUtils"
    "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include"
    "${REPO_ROOT}/freeink-sdk/libs/network/CatalogList/include"
    "${REPO_ROOT}/freeink-sdk/libs/network/CatalogList/src"
    "${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include"
    "${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include"
    "${CMAKE_CURRENT_LIST_DIR}/../pubsubclient/src"
    "${CMAKE_CURRENT_LIST_DIR}/../qrcode/src")

# Private JJWXC component: never part of the public tree. Opt in with
# CPYOS_PLUS_JJWXC=1 after cloning the private repository into src/jjwxc (the
# same contract as the public readpico_jjwxc environment). Without the flag this
# component builds exactly as before, with the booksource stubs.
set(CPYOS_JJWXC_DIR "${REPO_ROOT}/src/jjwxc")
set(CPYOS_JJWXC_ENABLED 0)
if(DEFINED ENV{CPYOS_PLUS_JJWXC})
    if(NOT IS_DIRECTORY "${CPYOS_JJWXC_DIR}")
        message(FATAL_ERROR
            "CPYOS_PLUS_JJWXC=1 but ${CPYOS_JJWXC_DIR} is missing; clone "
            "Yana-Hangabina/cpyos-jjwxc there (it must never be committed)")
    endif()
    # JjwxcSource.cpp is the ISource adapter the public BookSource registry
    # calls; it lives at the component root, not under app/.
    file(GLOB JJWXC_SRCS "${CPYOS_JJWXC_DIR}/app/*.cpp" "${CPYOS_JJWXC_DIR}/protocol/src/*.cpp"
         "${CPYOS_JJWXC_DIR}/JjwxcSource.cpp")
    list(LENGTH JJWXC_SRCS JJWXC_SRC_COUNT)
    if(JJWXC_SRC_COUNT EQUAL 0)
        message(FATAL_ERROR "CPYOS_PLUS_JJWXC=1 but no sources were found under ${CPYOS_JJWXC_DIR}")
    endif()
    list(APPEND APP_SOURCES ${JJWXC_SRCS})
    list(APPEND APP_INCLUDES "${CPYOS_JJWXC_DIR}/app" "${CPYOS_JJWXC_DIR}/protocol/src")
    set(CPYOS_JJWXC_ENABLED 1)
    message(STATUS "CPYOS Plus: private JJWXC component enabled (${JJWXC_SRC_COUNT} sources)")
endif()

idf_component_register(
    SRCS ${APP_SOURCES}
    INCLUDE_DIRS ${APP_INCLUDES}
    REQUIRES cpyos_ui cpyos_books cpyos_cellular freeink_font ArduinoJson pubsubclient qrcode esp_http_client
             # AI-voice client (plus_voice_client.cpp / plus_voice_activity.cpp).
             # Namespaced build names of the managed components declared in
             # main/idf_component.yml; esp-opus is listed explicitly because
             # opus_encoder.h pulls <opus.h> into our translation unit.
             espressif__esp_websocket_client espressif__cjson 78__esp-opus-encoder 78__esp-opus
             esp_psram nvs_flash esp_hw_support)

# Vendor OTA (CloudZao Burner). The personal key lives in a gitignored file so
# it is never committed or logged; without it the page says this build has no
# credentials instead of failing an authenticated request.
set(PLUS_OTA_KEY_FILE "${CMAKE_CURRENT_LIST_DIR}/plus_ota_key.local")
if(EXISTS "${PLUS_OTA_KEY_FILE}")
    file(READ "${PLUS_OTA_KEY_FILE}" PLUS_OTA_KEY_VALUE)
    string(STRIP "${PLUS_OTA_KEY_VALUE}" PLUS_OTA_KEY_VALUE)
    if(PLUS_OTA_KEY_VALUE)
        message(STATUS "CPYOS Plus: vendor OTA key loaded from plus_ota_key.local")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE PLUS_OTA_KEY="${PLUS_OTA_KEY_VALUE}")
    endif()
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO=10800
    CPYOS_PLUS_VENDOR_OTA=1
    FREEINK_DEVICE_READPICO=1
    SIMULATOR_DEVICE_READPICO=1
    BOARD_HAS_PSRAM=1
    # Activity-layer LOG_* were compiled out without these: the browser and
    # JJWXC reported nothing at all on the console. DBG is kept for bring-up;
    # lower to 1 for a quieter product build.
    ENABLE_SERIAL_LOG
    LOG_LEVEL=2
    CROSSPOINT_VECTOR_FONTS=1
    ENABLE_CHINESE_VERSION=1
    USE_UTF8_LONG_NAMES=1
    DESTRUCTOR_CLOSES_FILE=1
    CROSSPOINT_VERSION="0.3.1-plus-test1"
    CROSSPOINT_PORT_NO_NETWORK_SHELL=1
    CPYOS_OTA_DISABLED=1
    CPYOS_FONT_EXTRAS_MANIFEST_URL="https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt"
    CPYOS_FONT_EXTRAS_MANIFEST_RAW_URL="https://raw.githubusercontent.com/Yana-Hangabina/cpyos/cpyos/site/fonts/manifest-v1.txt")

if(CPYOS_JJWXC_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CROSSPOINT_BOOKSOURCE_JJWXC=1)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include Arduino.h>
    -Wno-error -Wno-narrowing -Wno-format -Wno-unused-parameter -Wno-sign-compare
    -Wno-error=missing-field-initializers -Wno-error=maybe-uninitialized
    -Wno-error=class-memaccess -Wno-error=stringop-overflow -Wno-error=comment)
