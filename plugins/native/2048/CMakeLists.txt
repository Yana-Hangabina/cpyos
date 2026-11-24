cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plugin2048)

# Only the ELF target is meaningful here; the normal app image is not shipped.
include("${CMAKE_CURRENT_LIST_DIR}/../../../ports/metalio_plus/components/elf_loader/elf_loader.cmake")
project_elf(plugin2048)
