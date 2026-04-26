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
if (DEFINED REPLICATION)
  if (${REPLICATION} STREQUAL "ON")
    find_package(NuraftMesg QUIET REQUIRED)
  endif()
endif()

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

list(APPEND COMMON_TEST_DEPS
    ${COMMON_DEPS}
    ${spdk_LIBRARY_LIST}
    ${dpdk_LIBRARY_LIST}
    GTest::gmock
)

include_directories (BEFORE ${CMAKE_CURRENT_SOURCE_DIR})  # enables <sisl/...>, <iomanager/...>, <homestore/...>
include_directories (BEFORE include/)
include_directories (BEFORE ${CMAKE_CURRENT_BINARY_DIR}/homestore)

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
# TODO: re-enable remaining modules
# add_subdirectory(homestore/index)

# if (DEFINED REPLICATION)
#   if (${REPLICATION} STREQUAL "ON")
#     add_subdirectory(homestore/replication/)
#   endif()
# endif()

# TODO: re-enable top-level tests once all modules are ported
# if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
#     add_subdirectory(tests)
# endif()

# TODO: re-enable homestore aggregate library once all object targets exist
# set(HOMESTORE_OBJECTS
#     $<TARGET_OBJECTS:hs_common>
#     $<TARGET_OBJECTS:hs_device>
#     $<TARGET_OBJECTS:hs_blkalloc>
#     $<TARGET_OBJECTS:hs_logdev>
#     $<TARGET_OBJECTS:hs_metablk>
#     $<TARGET_OBJECTS:hs_checkpoint>
#     $<TARGET_OBJECTS:hs_index>
#     $<TARGET_OBJECTS:hs_mem_btree>
#     $<TARGET_OBJECTS:hs_cow_btree>
#     $<TARGET_OBJECTS:hs_blob>
#     homestore/homestore.cpp
#     homestore/crc.cpp
#     homestore/managers.cpp
# )
# add_library(homestore STATIC ${HOMESTORE_OBJECTS})
# target_compile_definitions(homestore PRIVATE LOG_MODS_V2_SUPPORT)
# target_link_libraries(homestore PRIVATE ${COMMON_DEPS})
