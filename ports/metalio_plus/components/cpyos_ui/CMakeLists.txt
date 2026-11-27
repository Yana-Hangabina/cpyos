if(NOT DEFINED ENV{CPYOS_UI_PROBE} AND NOT DEFINED ENV{CPYOS_READER_DEMO})
    # Keep the working BSP build green until the UI compatibility layer is ready.
    idf_component_register()
    return()
endif()

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")

# English + Chinese stay in the image; every other language is written to
# build/i18n_pack.dat and read from the spiffs partition at runtime (the same
# split Read Pico uses). Without the pack the UI still runs on EN/ZH and the
# other languages are reported unavailable.
set(PORT_I18N_DIR "${CMAKE_CURRENT_BINARY_DIR}/port_i18n")
file(MAKE_DIRECTORY "${PORT_I18N_DIR}")
execute_process(
    COMMAND python3 "${REPO_ROOT}/scripts/gen_i18n.py" "${REPO_ROOT}/lib/I18n/translations"
            "${PORT_I18N_DIR}" --builtin-langs en,zh_cn
            --pack "${CMAKE_BINARY_DIR}/i18n_pack.dat"
    RESULT_VARIABLE PORT_I18N_RESULT
    OUTPUT_QUIET)
if(NOT PORT_I18N_RESULT EQUAL 0)
    message(FATAL_ERROR "port i18n generation failed (${PORT_I18N_RESULT})")
endif()
# The generated keys header declares only the languages that are compiled in;
# I18n.h/I18n.cpp include it with angle brackets so this copy wins over the
# all-language one in lib/I18n.

idf_component_register(
    SRCS
        "${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp"
        "${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp"
        "${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp"
        "${REPO_ROOT}/lib/Logging/Logging.cpp"
        "${REPO_ROOT}/lib/Utf8/Utf8.cpp"
        "${REPO_ROOT}/lib/EpdFont/EpdFont.cpp"
        "${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp"
        "${REPO_ROOT}/lib/EpdFont/TtfEpdFont.cpp"
        "${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp"
        "${REPO_ROOT}/lib/EpdFont/SdCardFontCache.cpp"
        "${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp"
        "${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp"
        "${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp"
        # FontDecompressor inflates the built-in compressed font groups, so the
        # inflater belongs to the UI component. It used to be compiled only into
        # cpyos_books, which made this a dependency cycle: the linker scanned
        # cpyos_books before pulling FontDecompressor out of libcpyos_ui.a and
        # reported InflateReader::* as undefined.
        "${REPO_ROOT}/lib/InflateReader/InflateReader.cpp"
        "${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp"
        "${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/compat/HalFrontlight.cpp"
        "${REPO_ROOT}/lib/hal/HalTiltSensor.cpp"
        "${REPO_ROOT}/freeink-sdk/libs/hardware/FrontlightManager/src/FrontlightManager.cpp"
        "${REPO_ROOT}/freeink-sdk/libs/hardware/Imu/src/Imu.cpp"
        "${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp"
        "${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp"
        "${REPO_ROOT}/lib/MiniBidi/minibidi.c"
        "${REPO_ROOT}/src/ReaderFontSizes.cpp"
        "${REPO_ROOT}/lib/I18n/I18n.cpp"
        "${REPO_ROOT}/lib/I18n/I18nPack.cpp"
        "${PORT_I18N_DIR}/I18nStrings.cpp"
        "${REPO_ROOT}/src/CrossPointSettings.cpp"
        "${REPO_ROOT}/src/RecentBooksStore.cpp"
        "${REPO_ROOT}/src/util/TimeUtils.cpp"
        "${REPO_ROOT}/src/booksource/BookSource.cpp"
        "${REPO_ROOT}/lib/SloppyDigits/SloppyDigits.cpp"
        "${REPO_ROOT}/lib/SloppyDigits/SloppyDigitAlphabets.cpp"
        "${REPO_ROOT}/src/MappedInputManager.cpp"
        "${REPO_ROOT}/src/SdCardFontSystem.cpp"
        # Vector system face: the built-in bitmap families are 2-bit (four grey
        # levels), so on this 16-level panel they cannot look smooth. The Plus
        # ships the same CPYOS Han Sans pack Read Pico embeds, inflated once to
        # SD and streamed glyph-by-glyph out of PSRAM.
        "${REPO_ROOT}/src/SystemFontPack.cpp"
        "${REPO_ROOT}/src/SystemCoreFont.cpp"
        "${REPO_ROOT}/src/UiScriptFont.cpp"
        "${REPO_ROOT}/src/Typography.cpp"
        "${REPO_ROOT}/src/components/UITheme.cpp"
        "${REPO_ROOT}/src/components/themes/BaseTheme.cpp"
        "${REPO_ROOT}/src/components/themes/cpy/CpyTheme.cpp"
        "${REPO_ROOT}/src/components/themes/upstream/UpstreamStyleTheme.cpp"
        "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp"
        "${REPO_ROOT}/lib/Serialization/PersistableStore.cpp"
        "${REPO_ROOT}/lib/KOReaderSync/KOReaderCredentialStore.cpp"
        "${REPO_ROOT}/lib/Serialization/ObfuscationUtils.cpp"
        "${REPO_ROOT}/lib/hal/HalMemory.cpp"
        "${REPO_ROOT}/lib/hal/HalOtaSlot.cpp"
        "${REPO_ROOT}/lib/SoundFeedback/SoundFeedback.cpp"
        "compat/HalAudioOutput.cpp"
        "${REPO_ROOT}/lib/Memory/BuildScratch.cpp"
        # Shared invariant owners. Both are called from the portable tree
        # (FirmwareFlasher/SystemFontPack/PinyinDb/... ask memory::internalFlashAllowed();
        # themes, ButtonNavigator and the parsers route callbacks through callbacks::),
        # so listing only their headers would link-fail this port. A stub branch
        # that omits a symbol is not a missing feature: it is an undefined
        # reference at link (the same trap as the C3 SystemCoreFont stub).
        "${REPO_ROOT}/lib/Memory/StackSafety.cpp"
        # The guard records a fault code at every refusal, so the recorder is part
        # of every target that links the guard.
        "${REPO_ROOT}/lib/Diagnostics/FaultLog.cpp"
        "${REPO_ROOT}/lib/Callbacks/InvokeCallback.cpp"
        "HalDisplay.cpp"
        "HalStorage.cpp"
        "HalClock.cpp"
        "HalPowerManager.cpp"
        "HalSystem.cpp"
        "HalGPIO.cpp"
        "stubs.cpp"
        "base64.cpp"
        "MD5Builder.cpp"
        "wolfcrypt_shim.cpp"
        "Preferences.cpp"
        "Wire.cpp"
        "ble_keyboard_host.cpp"
        "SecureClient_port.cpp"
        "net_selftest.cpp"
        "compat/WiFi.cpp"
        "compat/WebServer.cpp"
        "compat/WiFiClient.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/compat"
        "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/include"
        "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/include"
        "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/services/gap/include"
        "${IDF_PATH}/components/bt/host/nimble/nimble/porting/nimble/include"
        "${IDF_PATH}/components/bt/host/nimble/nimble/porting/npl/freertos/include"
        "${IDF_PATH}/components/bt/host/nimble/port/include"
        "${IDF_PATH}/components/bt/host/nimble/esp-hci/include"
        "${REPO_ROOT}/lib/JsonParser"
        "${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include"
        "${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src"
        "${REPO_ROOT}/freeink-sdk/libs/hardware/Imu/include"
        # Imu.cpp includes <I2cBusLock.h> from the SDK's BoardConfig component;
        # this port uses the compat/BoardConfig.h profile, so the real header
        # still has to be on the include path.
        "${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include"
        "${REPO_ROOT}/freeink-sdk/libs/hardware/Rtc/include"
        "${REPO_ROOT}/freeink-sdk/libs/hardware/FrontlightManager/include"
        "${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include"
        "${REPO_ROOT}/lib/EpdFont"
        "${REPO_ROOT}/lib/GfxRenderer"
        "${REPO_ROOT}/lib/miniz/src"
        "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include"
        "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src"
        "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb"
        "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/freetype/include"
        "${REPO_ROOT}/lib/InflateReader"
        "${REPO_ROOT}/lib/uzlib/src"
        "${REPO_ROOT}/lib/MiniBidi"
        "${REPO_ROOT}/lib/Utf8"
        "${REPO_ROOT}/lib/Memory"
        "${REPO_ROOT}/lib/Diagnostics"
        "${REPO_ROOT}/lib/Callbacks"
        "${REPO_ROOT}/lib/Logging"
        "${PORT_I18N_DIR}"
        "${REPO_ROOT}/lib/I18n"
        "${REPO_ROOT}/lib/Serialization"
        "${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include"
        "${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include"
        "${REPO_ROOT}/lib/FsHelpers"
        "${REPO_ROOT}/lib/SloppyDigits"
        "${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include"
        "${REPO_ROOT}/lib/Epub"
        "${REPO_ROOT}/lib/KOReaderSync"
        "${REPO_ROOT}/lib/HttpHeader"
        "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include"
        "${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src"
        "${REPO_ROOT}/src"
        "${REPO_ROOT}/lib/hal"
    REQUIRES
        cpyos_metalio_bsp cpyos_cellular app_update esp_partition freeink_font ArduinoJson mbedtls nvs_flash esp_wifi esp_netif esp_event esp-tls lwip driver esp_driver_i2c esp_driver_ledc efuse esp_psram esp_timer esp_hw_support heap log esp_http_server
        freertos spi_flash vfs fatfs bt
    # UI feedback cues. Same two WAVs Read Pico embeds; local copies keep the
    # generated _binary_assets_sounds_* symbol names the shared code expects.
    EMBED_FILES "assets/sounds/select.wav" "assets/sounds/tap.wav")

# PUBLIC: I18n.h's class layout depends on this macro, so every consumer of the
# header (app, books, main) must see the same value.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPOINT_I18N_PACK=1)
# The Plus has a cool/warm LED pair on GPIO6/GPIO7 (vendor config.h). Keeping the
# capability PUBLIC means SettingsList and the HAL agree on the warmth row.
target_compile_definitions(${COMPONENT_LIB} PUBLIC FREEINK_CAP_WARMLIGHT=1)
# BLE HID host: the port supplies its own NimBLE backend behind this capability.
target_compile_definitions(${COMPONENT_LIB} PUBLIC FREEINK_CAP_BLE_HID_HOST=1)
# Haptics: the motor is on TCA9555 P0.3, so the SDK's GPIO haptic backend is not
# used; the port's HalGPIO drives it and SettingsList exposes the level row.
target_compile_definitions(${COMPONENT_LIB} PUBLIC FREEINK_CAP_HAPTIC=1)
# Speaker feedback: the BH1098G path in plus_audio.c provides PCM playback, so
# the shared SoundFeedback cues and the "sound feedback" setting become real.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPOINT_CAP_SOUND_FEEDBACK=1)
# Page-turn effect: the port maps Read Pico's water ripple onto the vendor's
# own spatial wipe (MODE_SPATIAL_WIPE_GL: ten GL16 bands, each one phase later).
# See HalDisplay::setPageTurnDirection; the setting therefore stays visible.
# 刷新屏幕 must issue a non-differential GC16 sweep on this panel (see
# FrontlightPanelActivity): HALF is differential and cannot clear residue.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPOINT_PORT_CLEAN_USES_FULL=1)
# Column-band diff rendering is enabled: displayWindow now performs a real
# epd_hl_update_area push (verified with 20 windowed scans, err=0) behind a 3 s
# scan watchdog. Without this macro GfxRenderer pushes only the changed band.
# A fetch task that will not stop must not be deleted mid-heap on this port.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPOINT_PORT_STUCK_JOB_RESTART=1)
# Light sleep is implemented (vendor TPS/bus/expander/EXT1 sequence) and its
# style/image rows stay visible. Deep sleep is not: the vendor's "off" is the
# P1.3 power-off pulse, so hide only the deep-sleep rows.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPOINT_PORT_NO_DEEP_SLEEP_SCREENS=1)

# These change public class layouts (FontCacheManager, SdCardFontSystem, ...).
# They MUST be PUBLIC: a dependent TU compiled without them builds a smaller
# object and the library's constructor writes past its end. That is exactly how
# the shell's activityManager vptr was being zeroed at boot.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BOARD_HAS_PSRAM=1
    ENABLE_CHINESE_VERSION=1
    CROSSPOINT_VECTOR_FONTS=1
    CROSSPOINT_SYSTEM_FONT_PACK=1
    # English + Chinese stay in the image; the other 33 languages live in
    # build/i18n_pack.dat and are read from the spiffs partition (the port uses
    # the same 16 MiB table as Read Pico, so the pack sits at spiffs +0x100000).
    CROSSPOINT_I18N_PACK=1
    CROSSPOINT_FONT_PACK_EMBEDDED=1
    USE_UTF8_LONG_NAMES=1
    CROSSMUX_UI_PROFILE_HIGH_DPI=1
    CROSSPOINT_PLUS_PORT=1
    CROSSPOINT_BLE_HOST_PSRAM=1
    CPYOS_PLUS_BOARD_PROFILE=1
    FREEINK_CAP_TOUCH=1)

# BookSource.cpp in this component is the public registry; when the private
# component is part of the build it must resolve booksource::privateSource()
# instead of compiling the null stub. The symbol itself lives in cpyos_app.
# Keep the same auto-detection as cpyos_app: the two component CMake files are
# configured independently, so an explicit env flag is not the only signal.
set(_cpyos_ui_jjwxc 0)
if(DEFINED ENV{CPYOS_PLUS_JJWXC})
    if(NOT "$ENV{CPYOS_PLUS_JJWXC}" STREQUAL "0")
        set(_cpyos_ui_jjwxc 1)
    endif()
elseif(IS_DIRECTORY "${REPO_ROOT}/src/jjwxc")
    set(_cpyos_ui_jjwxc 1)
endif()
if(_cpyos_ui_jjwxc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CROSSPOINT_BOOKSOURCE_JJWXC=1)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO=10800
    FREEINK_DEVICE_READPICO=1
    CPYOS_FONT_EXTRAS_MANIFEST_URL="https://yana-hangabina.github.io/cpyos/fonts/manifest-v1.txt"
    CPYOS_FONT_EXTRAS_MANIFEST_RAW_URL="https://raw.githubusercontent.com/Yana-Hangabina/cpyos/cpyos/site/fonts/manifest-v1.txt"
    SIMULATOR_DEVICE_READPICO=1
    EINK_DISPLAY_SINGLE_BUFFER_MODE=1
    DESTRUCTOR_CLOSES_FILE=1
    ENABLE_SERIAL_LOG
    LOG_LEVEL=2
    MAX_WIDTH=3072
    PNG_MAX_BUFFERED_PIXELS=16416
    XML_GE=0
    XML_CONTEXT_BYTES=0
    CROSSPOINT_VERSION="0.3.1-plus-test1")

# assets/fonts/cpyos_han_sans.def -> _binary_assets_fonts_cpyos_han_sans_def_{start,end}
target_add_binary_data(${COMPONENT_LIB} "${REPO_ROOT}/assets/fonts/cpyos_han_sans.def" BINARY
    RENAME_TO "assets_fonts_cpyos_han_sans_def")
# Non-CJK UI scripts (Vietnamese/Czech/Polish/Turkish/Hebrew/Arabic), ~25 KB.
target_add_binary_data(${COMPONENT_LIB} "${REPO_ROOT}/assets/fonts/cpyos_ui_ext.def" BINARY
    RENAME_TO "assets_fonts_cpyos_ui_ext_def")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-narrowing -Wno-format -Wno-error=unused-but-set-parameter
    -Wno-error=maybe-uninitialized -Wno-error=array-bounds -Wno-error=comment -Wno-error=missing-field-initializers -Wno-error=class-memaccess -Wno-error=stringop-overflow)
