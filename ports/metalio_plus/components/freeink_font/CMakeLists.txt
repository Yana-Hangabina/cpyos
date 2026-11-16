# FreeInkFont + the vendored FreeType build that backs CPYOS's runtime TTF/OTF
# rendering (lib/EpdFont/TtfEpdFont). This is the same source tree the
# PlatformIO build compiles; IDF just needs the include paths and the module
# set spelled out. The auto-hinter is required by TtfEpdFont's 2-bit quantizer.
set(FONT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../freeink-sdk/libs/font/FreeInkFont")
set(FT_DIR "${FONT_DIR}/third_party/freetype")

# src/freetype/*.c are the library's generated FreeType build wrappers: each one
# defines FT2_BUILD_LIBRARY and #includes exactly one amalgam (ftbase.c,
# autofit.c, ...). Compiling the amalgams a second time duplicates symbols, and
# compiling the individual module sources duplicates them again — so this list
# must stay on the wrappers, exactly like the PlatformIO build.
file(GLOB FONT_SRCS "${FONT_DIR}/src/*.c" "${FONT_DIR}/src/*.cpp" "${FONT_DIR}/src/freetype/*.c")

idf_component_register(
    SRCS ${FONT_SRCS}
    INCLUDE_DIRS
        "${FONT_DIR}/include"
        "${FT_DIR}/include"
        "${FONT_DIR}/third_party/stb"
    REQUIRES heap log)

# A/B: autofit crashed in af_latin_hints_compute_segments (NULL
# prev_segment->last) while rendering the WeRead screen on this target, so the
# auto-hinter is disabled here. TtfEpdFont then keeps its default hinting
# (unhinted outlines), which the 50% BW quantizer and the 4-bit coverage path
# both handle; re-enable only after the FreeType bug is fixed and verified.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    FREEINK_FONT_ENABLE_AUTOHINT=0
    FREEINK_FONT_ENABLE_CFF=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format -Wno-error -Wno-comment -Wno-unused-parameter -Wno-sign-compare)
