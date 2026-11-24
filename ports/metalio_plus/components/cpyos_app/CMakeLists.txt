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
file(GLOB NATIVE_PLUGIN_SRCS "${REPO_ROOT}/src/plugins/*.cpp")

# Already compiled by cpyos_ui/cpyos_books, or not portable yet.
# Native plugin builds ship the games as ELF packages; exclude the in-tree
# game activities so the firmware image actually shrinks.
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/apps/(2048|sokoban|gomoku|minesweeper|sudoku|chinese-chess)/")
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/network/CrossPointWebServerActivity\\.cpp$")
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/network/CalibreConnectActivity\\.cpp$")
# Read Pico's shutdown-confirm activity calls BoardReadPico's PMU handoff; the
# Plus owns its own power card (power_off_activity.cpp) and never pushes this.
list(FILTER ACTIVITY_SRCS EXCLUDE REGEX "/PowerOffActivity\\.cpp$")
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
file(GLOB VOCAB_SRCS "${REPO_ROOT}/src/vocabulary/*.cpp")

set(APP_SOURCES
    "bluetooth_activity_stub.cpp"
    "plus_bt_audio_activity.cpp"
    "plus_camera_activity.cpp"
    "plus_transfer_activity.cpp"
    "plus_transfer_hooks.cpp"
    "plus_ota.cpp"
    "plus_ota_activity.cpp"
    "plus_resources.cpp"
    "plus_resources_activity.cpp"
    "${REPO_ROOT}/lib/BmpToBmpConverter/BmpToBmpConverter.cpp"
    "${REPO_ROOT}/lib/HttpHeader/HttpHeader.cpp"
    "plus_voice_activity.cpp"
    "plus_voice_client.cpp"
    "plus_voice_memo_activity.cpp"
    "power_off_activity.cpp"
    "ui_font_refresh.cpp"
    ${VOCAB_SRCS}
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
    ${CATALOG_SRCS}
    ${NATIVE_PLUGIN_SRCS})

# PDF reader.  The engine itself is NOT linked: it is a MuPDF module built for
# RISC-V from the same firmware ELF (scripts/build_pdf_module.py) and loaded
# from the SD card into PSRAM.  CPYOS_PDF=1 at configure time turns the reader
# on; the default shell keeps the PDF code out of the image.
set(CPYOS_PDF_ENABLED 0)
if(DEFINED ENV{CPYOS_PDF} AND NOT "$ENV{CPYOS_PDF}" STREQUAL "0")
    set(CPYOS_PDF_ENABLED 1)
endif()
if(CPYOS_PDF_ENABLED)
    list(APPEND APP_SOURCES
        "${REPO_ROOT}/src/pdf/PdfAssets.cpp"
        "${REPO_ROOT}/src/pdf/PdfAssetDownload.cpp"
        "${REPO_ROOT}/src/pdf/PdfFontCache.cpp"
        "${REPO_ROOT}/src/pdf/PdfDocument.cpp"
        "${REPO_ROOT}/src/pdf/PdfDocumentXip.cpp"
        "${REPO_ROOT}/src/pdf/PdfModuleEntry.cpp"
        "${REPO_ROOT}/src/pdf/PdfModuleRuntime.cpp"
        "${REPO_ROOT}/src/pdf/PdfModuleRuntimeSd.cpp"
        "${REPO_ROOT}/src/pdf/PdfModuleHostServices.cpp"
        "${REPO_ROOT}/src/pdf/PdfEngineUpdate.cpp")
    message(STATUS "CPYOS Plus: PDF reader enabled (SD engine)")
endif()

set(APP_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../cpyos_ui/compat"
    "${REPO_ROOT}/src"
    "${REPO_ROOT}/src/activities"
    "${REPO_ROOT}/src/vocabulary"
    "${REPO_ROOT}/src/components"
    "${REPO_ROOT}/src/plugins"
    "${REPO_ROOT}/lib/AvatarGen"
    "${REPO_ROOT}/lib/BmpToBmpConverter"
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
    "${REPO_ROOT}/lib/miniz/src"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/Imu/include"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/Rtc/include"
    "${REPO_ROOT}/freeink-sdk/libs/hardware/FrontlightManager/include"
    "${REPO_ROOT}/lib/HttpHeader"
    "${REPO_ROOT}/lib/AsciiText"
    "${REPO_ROOT}/src/pdf"
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

# Private JJWXC component: never part of the public tree. A dev checkout at
# src/jjwxc is enabled automatically so a normal local shell build is not a
# build with a visible-but-dead JJWXC tile. Set CPYOS_PLUS_JJWXC=0 to opt out,
# or =1 to require it (the build then fails loudly if the checkout is missing).
set(CPYOS_JJWXC_DIR "${REPO_ROOT}/src/jjwxc")
set(CPYOS_JJWXC_ENABLED 0)
set(CPYOS_JJWXC_REQUESTED 0)
if(DEFINED ENV{CPYOS_PLUS_JJWXC})
    if("$ENV{CPYOS_PLUS_JJWXC}" STREQUAL "0")
        set(CPYOS_JJWXC_REQUESTED 0)
    else()
        set(CPYOS_JJWXC_REQUESTED 1)
    endif()
elseif(IS_DIRECTORY "${CPYOS_JJWXC_DIR}")
    set(CPYOS_JJWXC_REQUESTED 1)
    message(STATUS "CPYOS Plus: private JJWXC checkout found; enabling it automatically")
endif()
if(CPYOS_JJWXC_REQUESTED)
    if(NOT IS_DIRECTORY "${CPYOS_JJWXC_DIR}")
        message(FATAL_ERROR
            "CPYOS_PLUS_JJWXC is enabled but ${CPYOS_JJWXC_DIR} is missing; clone "
            "Yana-Hangabina/cpyos-jjwxc there (it must never be committed)")
    endif()
    # JjwxcSource.cpp is the ISource adapter the public BookSource registry
    # calls; it lives at the component root, not under app/.
    file(GLOB JJWXC_SRCS "${CPYOS_JJWXC_DIR}/app/*.cpp" "${CPYOS_JJWXC_DIR}/protocol/src/*.cpp"
         "${CPYOS_JJWXC_DIR}/JjwxcSource.cpp")
    list(LENGTH JJWXC_SRCS JJWXC_SRC_COUNT)
    if(JJWXC_SRC_COUNT EQUAL 0)
        message(FATAL_ERROR "CPYOS Plus: private JJWXC component has no sources under ${CPYOS_JJWXC_DIR}")
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
             # Native ELF plugin loader (components/elf_loader). The plugin host
             # only exists when CROSSPOINT_PLUGIN_RUNTIME is defined below.
             elf_loader
             # AI-voice client (plus_voice_client.cpp / plus_voice_activity.cpp).
             # Namespaced build names of the managed components declared in
             # main/idf_component.yml; esp-opus is listed explicitly because
             # opus_encoder.h pulls <opus.h> into our translation unit.
             espressif__esp_websocket_client espressif__cjson 78__esp-opus-encoder 78__esp-opus
             esp_psram nvs_flash esp_hw_support)

# Developer resource key (CloudZao). Protocol 1.1 uses this one credential for
# the resource popup and the community firmware OTA check (X-Resource-Key); there
# is no separate X-OTA-Key on the Plus. Kept in a gitignored file so it is never
# committed or logged.
set(PLUS_RESOURCE_KEY_FILE "${CMAKE_CURRENT_LIST_DIR}/plus_resource_key.local")
if(EXISTS "${PLUS_RESOURCE_KEY_FILE}")
    file(READ "${PLUS_RESOURCE_KEY_FILE}" PLUS_RESOURCE_KEY_VALUE)
    string(STRIP "${PLUS_RESOURCE_KEY_VALUE}" PLUS_RESOURCE_KEY_VALUE)
    if(PLUS_RESOURCE_KEY_VALUE)
        message(STATUS "CPYOS Plus: developer resource key loaded from plus_resource_key.local")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE PLUS_RESOURCE_KEY="${PLUS_RESOURCE_KEY_VALUE}")
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
    CROSSPOINT_VERSION="${PROJECT_VER}"
    CROSSPOINT_PORT_NO_NETWORK_SHELL=1
    # Native ELF plugin host: the shell's main component already defines this;
    # the shared activity layer needs the same view or ActivityManager keeps
    # the built-in game calls that this component deliberately excludes.
    CROSSPOINT_PLUGIN_RUNTIME=1
    # Shared CrossMux OTA manifests are Read Pico only. The Plus uses plus_ota
    # (CloudZao protocol 1.1) and must not consume those manifests.
    CPYOS_OTA_DISABLED=1
    CPYOS_FONT_EXTRAS_MANIFEST_URL="https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt"
    CPYOS_FONT_EXTRAS_MANIFEST_RAW_URL="https://raw.githubusercontent.com/Yana-Hangabina/cpyos/cpyos/site/fonts/manifest-v1.txt")

if(CPYOS_JJWXC_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CROSSPOINT_BOOKSOURCE_JJWXC=1)
endif()

if(CPYOS_PDF_ENABLED)
    # Target-specific engine manifest: one line, appTag = ELF SHA of the paired
    # app.  Releases pin the exact tag's asset; the default points at the
    # newest stable release.  CI overrides it so a test build updates from its
    # own release rather than from "latest".
    # Rolling index release: the asset is replaced on every engine publish, so
    # a device compiled today keeps finding engines for its own compatibility
    # class tomorrow.  Never point this at a single version's asset.
    set(CPYOS_PDF_MANIFEST_URL
        "https://github.com/Yana-Hangabina/cpyos/releases/download/pdf-engine/plus-manifest-v2.txt")
    if(DEFINED ENV{CPYOS_PDF_ENGINE_MANIFEST_URL})
        set(CPYOS_PDF_MANIFEST_URL "$ENV{CPYOS_PDF_ENGINE_MANIFEST_URL}")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CROSSPOINT_MUPDF=1
        CROSSPOINT_PDF_XIP=1
        CROSSPOINT_PDF_XIP_SD=1
        "CPYOS_PDF_ENGINE_MANIFEST_URL=\"${CPYOS_PDF_MANIFEST_URL}\"")
    # The host table references the font-cache and CMap services, so they stay
    # linked; no --undefined= pins are needed any more.
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include Arduino.h>
    -Wno-error -Wno-narrowing -Wno-format -Wno-unused-parameter -Wno-sign-compare
    -Wno-error=missing-field-initializers -Wno-error=maybe-uninitialized
    -Wno-error=class-memaccess -Wno-error=stringop-overflow -Wno-error=comment)
