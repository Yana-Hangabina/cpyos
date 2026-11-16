set(MAIN_SRCS "main.c")
set(MAIN_REQUIRES cpyos_metalio_bsp spi_flash esp_tinyusb bt app_update nvs_flash esp_partition)
if(DEFINED ENV{CPYOS_UI_PROBE})
    list(APPEND MAIN_SRCS "ui_probe.cpp")
    list(APPEND MAIN_REQUIRES cpyos_ui cpyos_books)
endif()
if(DEFINED ENV{CPYOS_READER_DEMO})
    list(APPEND MAIN_SRCS "reader_demo.cpp")
    list(APPEND MAIN_REQUIRES cpyos_ui cpyos_books)
endif()
if(DEFINED ENV{CPYOS_SHELL_DEMO})
    # Resolve the shared FilesPage templates (with their #include directives) into
    # a C++ literal served from flash. Re-run configure after editing the HTML.
    execute_process(
        COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../scripts/gen_web_pages.py"
                --out "${CMAKE_CURRENT_BINARY_DIR}/web_pages.inc"
        RESULT_VARIABLE web_pages_result)
    if(NOT web_pages_result EQUAL 0)
        message(FATAL_ERROR "gen_web_pages.py failed (${web_pages_result})")
    endif()
    list(APPEND MAIN_SRCS "shell_demo.cpp" "plus_ai_key.cpp" "plus_ai_key_sampler.cpp" "plus_ai_key_routing.cpp" "plus_log_mirror.cpp" "web_transfer.cpp" "web_plugins.cpp" "web_stores.cpp" "ws_upload.cpp" "webdav.cpp" "usb_msc.cpp")
    # esp_driver_usb_serial_jtag: the bench console disables the VFS CR->LF
    # translation so binary uploads survive (see shell_demo.cpp).
    list(APPEND MAIN_REQUIRES cpyos_ui cpyos_books cpyos_app esp_http_server esp_driver_usb_serial_jtag esp_driver_uart )
endif()

idf_component_register(SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/LibraryIndex"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/Epub"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/Epub/Epub"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/Pinyin"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/DeviceSecret"
    "${CMAKE_CURRENT_LIST_DIR}/../../../lib/SoundFeedback"
    "${CMAKE_CURRENT_LIST_DIR}/../../../src"
    "${CMAKE_CURRENT_LIST_DIR}/../../../src/util"
    "${CMAKE_CURRENT_LIST_DIR}/../../../src/network"
    REQUIRES ${MAIN_REQUIRES})
target_compile_definitions(${COMPONENT_LIB} PRIVATE CPYOS_PLUS_BSP=1 CPYOS_PLUS_BOARD_PROFILE=1 FREEINK_DEVICE_READPICO=1 ARDUINO=10800)

if(DEFINED ENV{CPYOS_UI_PROBE})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CPYOS_UI_PROBE=1)
    # FontExtrasActivity.h pulls FreeInkUIIcon.h, whose comment block ends a
    # line with a backslash; cpyos_ui already suppresses -Wcomment, and this
    # component now includes the same header.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers -Wno-error=comment)
endif()
if(DEFINED ENV{CPYOS_READER_DEMO})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CPYOS_READER_DEMO=1)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
endif()
if(DEFINED ENV{CPYOS_SHELL_DEMO})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CPYOS_SHELL_DEMO=1)
    # Bench/demo images: fresh settings start at sleep=never. The feature is
    # still usable from Settings; release packaging must NOT define this.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CPYOS_DEV_SLEEP_DEFAULT_NEVER=1)
    if(DEFINED ENV{CPYOS_PLUS_JJWXC})
        # The shell anchors the private booksource adapter (see shell_demo).
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CROSSPOINT_BOOKSOURCE_JJWXC=1)
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
endif()
