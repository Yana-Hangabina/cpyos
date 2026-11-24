cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello_plugin)

# The macro only adds the `elf` target; use `idf.py --preview elf` (the normal
# application image is not the deliverable here).
include("${CMAKE_CURRENT_LIST_DIR}/../../components/elf_loader/elf_loader.cmake")
project_elf(hello_plugin)
