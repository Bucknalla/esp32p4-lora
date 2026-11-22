cmake_minimum_required(VERSION 3.22)

# Pinned ESP-IDF release; see /.esp-idf-version and docs/adr/0001-esp-idf-version.md
set(LWGW_IDF_VERSION "6.1.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" STREQUAL "${LWGW_IDF_VERSION}")
    message(FATAL_ERROR "ESP-IDF ${LWGW_IDF_VERSION} required, found "
                        "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
endif()

idf_build_set_property(MINIMAL_BUILD ON)
project(lorawan_gateway)
