cmake_minimum_required(VERSION 3.11)

include_directories (BEFORE ../../)

add_library(hs_mem_btree OBJECT)
target_sources(hs_mem_btree PRIVATE
    mem_btree.cpp
    )
# hs_mem_btree is header-only logic + one .cpp. It doesn't depend on any other homestore OBJECT library —
# only third-party usage requirements (folly for ConcurrentHashMap/coroutines, fmt, boost, etc.) via COMMON_DEPS.
target_link_libraries(hs_mem_btree ${COMMON_DEPS})

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
