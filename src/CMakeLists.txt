cmake_minimum_required(VERSION 3.13)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)

find_package(Threads)
find_package(isa-l QUIET)
find_package(farmhash QUIET REQUIRED)
find_package(GTest QUIET REQUIRED)
find_package(folly QUIET REQUIRED)
find_package(spdlog QUIET REQUIRED)
find_package(fmt QUIET REQUIRED)
find_package(Boost QUIET REQUIRED)
find_package(nlohmann_json QUIET REQUIRED)
find_package(userspace-rcu QUIET REQUIRED)
find_package(Snappy QUIET REQUIRED)
find_package(flatbuffers QUIET REQUIRED)
find_package(cxxopts QUIET REQUIRED)
find_package(zmarok-semver QUIET REQUIRED)

# Memory allocator selection (driven by conanfile MALLOC_IMPL toolchain variable).  jemalloc/tcmalloc replace
# malloc/free symbols and so are mutually exclusive with ASAN — the conanfile auto-coerces to libc when sanitize=True.
if (DEFINED MALLOC_IMPL)
    if ("${MALLOC_IMPL}" STREQUAL "jemalloc")
        find_package(jemalloc QUIET REQUIRED)
        add_flags("-DUSE_JEMALLOC=1")
        list(APPEND MALLOC_LIBS jemalloc::jemalloc)
        message(STATUS "Memory allocator: jemalloc")
    elseif ("${MALLOC_IMPL}" STREQUAL "tcmalloc")
        find_package(gperftools QUIET REQUIRED)
        add_flags("-DUSING_TCMALLOC=1")
        list(APPEND MALLOC_LIBS gperftools::gperftools)
        message(STATUS "Memory allocator: tcmalloc")
    else ()
        message(STATUS "Memory allocator: libc (system)")
    endif ()
else ()
    message(STATUS "Memory allocator: libc (MALLOC_IMPL not set)")
endif ()

list(APPEND COMMON_DEPS
    farmhash::farmhash
    folly::folly
    spdlog::spdlog
    fmt::fmt
    Boost::boost
    nlohmann_json::nlohmann_json
    userspace-rcu::userspace-rcu
    Snappy::snappy
    flatbuffers::flatbuffers
    cxxopts::cxxopts
    zmarok-semver::zmarok-semver
)

if (DEFINED REPLICATION)
  if (${REPLICATION} STREQUAL "ON")
    list(APPEND COMMON_DEPS 
        nuraft_mesg::proto
        nuraft::nuraft
    )
  endif()
endif()

if (${isa-l_FOUND})
    list(APPEND COMMON_DEPS isa-l::isa-l)
else ()
    add_flags("-DNO_ISAL")
endif()

if (DEFINED MALLOC_LIBS)
    list(APPEND COMMON_DEPS ${MALLOC_LIBS})
endif()

list(APPEND COMMON_TEST_DEPS
    ${COMMON_DEPS}
    ${spdk_LIBRARY_LIST}
    ${dpdk_LIBRARY_LIST}
    GTest::gmock
)

include_directories (BEFORE ${CMAKE_CURRENT_SOURCE_DIR})  # enables "sisl/...", "iomanager/...", "homestore/..."
include_directories (BEFORE include/)
include_directories (BEFORE ${CMAKE_CURRENT_BINARY_DIR})  # enables "homestore/base/generated/..." (flatbuffers gen)

add_subdirectory(sisl)
add_subdirectory(iomanager)
add_subdirectory(homestore/base)
add_subdirectory(homestore/blkalloc)
add_subdirectory(homestore/device)
add_subdirectory(homestore/meta)
add_subdirectory(homestore/checkpoint)
add_subdirectory(homestore/blob)
add_subdirectory(homestore/logstore)
add_subdirectory(homestore/index)

# if (DEFINED REPLICATION)
#   if (${REPLICATION} STREQUAL "ON")
#     add_subdirectory(homestore/replication/)
#   endif()
# endif()

# Aggregate STATIC library — every per-module OBJECT lib plus the top-level orchestrator (homestore.cpp).
set(HOMESTORE_OBJECTS
    $<TARGET_OBJECTS:hs_base>
    $<TARGET_OBJECTS:hs_device>
    $<TARGET_OBJECTS:hs_blkalloc>
    $<TARGET_OBJECTS:hs_meta>
    $<TARGET_OBJECTS:hs_checkpoint>
    $<TARGET_OBJECTS:hs_blob>
    $<TARGET_OBJECTS:hs_logstore>
    $<TARGET_OBJECTS:hs_mem_btree>
    $<TARGET_OBJECTS:hs_cow_btree>
    homestore/homestore.cpp
)
add_library(homestore STATIC ${HOMESTORE_OBJECTS})
target_link_libraries(homestore PRIVATE ${COMMON_DEPS})
