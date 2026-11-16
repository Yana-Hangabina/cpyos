if(NOT DEFINED ENV{CPYOS_UI_PROBE} AND NOT DEFINED ENV{CPYOS_READER_DEMO})
    # Only the bring-up/probe and reader-demo images link the book engine today.
    idf_component_register()
    return()
endif()

# Vendored expat (lib/expat in this repository) for the book engine's XML
# parsers (container.xml, OPF, nav, XHTML).
file(GLOB EXPAT_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/expat/*.c")
idf_component_register(
    SRCS ${EXPAT_SRCS}
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/expat"
    REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    XML_STATIC=1
    XML_GE=0
    XML_CONTEXT_BYTES=0)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter -Wno-sign-compare)
