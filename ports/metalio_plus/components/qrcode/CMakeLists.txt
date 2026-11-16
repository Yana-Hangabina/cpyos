# ricmoo/QRCode (MIT), vendored from CPYOS's PlatformIO dependency tree.
file(GLOB QR_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.c" "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
idf_component_register(SRCS ${QR_SRCS} INCLUDE_DIRS "src" REQUIRES heap log)
# qrcode.h typedefs bool for C; GCC 15 defaults to C23 where bool is a keyword.
set_source_files_properties(${QR_SRCS} PROPERTIES COMPILE_FLAGS "-std=gnu17")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
