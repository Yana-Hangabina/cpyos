# ArduinoJson is a header-only MIT library vendored from the CPYOS
# PlatformIO dependency tree so this port builds without network access.
idf_component_register(INCLUDE_DIRS "src")

# Every consumer must see the same configuration: ArduinoJson puts a config
# hash in its inline namespace, so a TU compiled as "non-Arduino" (PROGMEM=0)
# cannot link against one compiled as Arduino (PROGMEM=1).
target_compile_definitions(${COMPONENT_LIB} INTERFACE
    ARDUINOJSON_ENABLE_PROGMEM=1
    ARDUINOJSON_ENABLE_ARDUINO_STRING=1)

