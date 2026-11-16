if(NOT DEFINED ENV{CPYOS_UI_PROBE} AND NOT DEFINED ENV{CPYOS_READER_DEMO})
    # Only the bring-up/probe and reader-demo images link the book engine today.
    idf_component_register()
    return()
endif()

# Vendored uzlib tinflate (lib/uzlib) used by InflateReader.
file(GLOB UZ_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/uzlib/src/*.c")
idf_component_register(
    SRCS ${UZ_SRCS}
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/uzlib/src"
    REQUIRES heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter -Wno-sign-compare)
