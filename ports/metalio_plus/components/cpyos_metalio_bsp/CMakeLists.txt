set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/board")

file(GLOB DISPLAY_SRCS
    "${BOARD_DIR}/display/*.c"
    "${BOARD_DIR}/display/output_common/*.c"
    "${BOARD_DIR}/display/output_lcd/*.c"
    "${BOARD_DIR}/display/waveforms/*.c")
file(GLOB BUS_SRCS "${BOARD_DIR}/bus/*.c")

idf_component_register(
    SRCS ${DISPLAY_SRCS} ${BUS_SRCS} "cpyos_sd.c" "plus_battery.c" "plus_rtc.c" "plus_charger.c"
         "plus_audio.c" "plus_camera.c"
    INCLUDE_DIRS
        "include"
        "${BOARD_DIR}"
        "${BOARD_DIR}/bus"
        "${BOARD_DIR}/display"
        "${BOARD_DIR}/display/waveforms"
        "${BOARD_DIR}/display/output_common"
        "${BOARD_DIR}/display/output_lcd"
    REQUIRES
        driver esp_driver_gpio esp_driver_i2c esp_driver_rmt esp_driver_dma
        esp_driver_sdmmc esp_driver_i2s esp_driver_uart esp_lcd esp_hal_lcd esp_hal_rmt
        esp_hal_uart esp_hal_i2s esp_hw_support esp_mm esp_timer esp_psram esp_rom
        # GC2145 still camera: the DVP capture and the hardware JPEG encoder.
        # Both share LCD_CAM with the panel; see plus_camera.h for the hand-off.
        esp_driver_cam esp_driver_jpeg
        sdmmc fatfs freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format -Wno-error=format-truncation)
