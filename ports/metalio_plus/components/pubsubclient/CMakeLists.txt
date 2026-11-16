# PubSubClient (MIT), vendored from the CPYOS PlatformIO dependency tree.
# AirPage uses it for MQTT; on the Plus the Wi-Fi client it needs is a compat
# stub until the IDF network stack is wired.
file(GLOB PS_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
idf_component_register(
    SRCS ${PS_SRCS}
    INCLUDE_DIRS "src" "${CMAKE_CURRENT_LIST_DIR}/../cpyos_ui/compat"
    REQUIRES heap log esp_psram esp_timer)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ARDUINO=10800)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include Arduino.h> -Wno-error)
