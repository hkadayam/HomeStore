cmake_minimum_required(VERSION 3.13)

include_directories (BEFORE ..)
include_directories (BEFORE .)

find_package(flatbuffers REQUIRED)

add_library(hs_base OBJECT)
target_sources(hs_base PRIVATE
      crc.cpp
      error.cpp
      homestore_status_mgr.cpp
      resource_mgr.cpp
    )
target_link_libraries(hs_base ${COMMON_DEPS})

set(FLATBUFFERS_FLATC_EXECUTABLE ${flatbuffers_INCLUDE_DIR}/../bin/flatc)
settings_gen_cpp(${FLATBUFFERS_FLATC_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/generated/ hs_base homestore_config.fbs)
