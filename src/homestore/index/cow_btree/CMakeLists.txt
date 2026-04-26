cmake_minimum_required(VERSION 3.11)

include_directories (BEFORE ../)

add_library(hs_cow_btree OBJECT)
target_sources(hs_cow_btree PRIVATE
    cow_btree_mgr.cpp
    cow_btree.cpp
    ../btree/btree_base.cpp
    )
# COWBtree streams (AppendBlkStream/RawBlkStream) always return folly::coro::Task<>; CO_AWAIT must be co_await,
# so this TU compiles in btree's async mode.  btree_base.cpp is compiled here (not in a shared hs_btree library)
# so it picks up the async BtreeSharedMutex / NodeCore layout — a single shared hs_btree would ODR-clash with
# the sync-mode mem_btree consumer.
target_compile_definitions(hs_cow_btree PRIVATE BTREE_ASYNC_MODE)
target_link_libraries(hs_cow_btree ${COMMON_DEPS})

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
