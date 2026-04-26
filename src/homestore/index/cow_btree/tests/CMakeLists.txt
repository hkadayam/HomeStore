cmake_minimum_required(VERSION 3.13)

# Include roots: <homestore/...> and <sisl/...> from src/, tests see homestore/index/btree/tests/ helpers.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../include)

add_executable(test_cow_btree_io test_cow_btree_io.cpp)
# COWBtree's stream APIs return folly::coro::Task<> regardless of mode; CO_AWAIT must be co_await, so this TU
# compiles in btree's async mode (matching hs_cow_btree's PUBLIC contract).
target_compile_definitions(test_cow_btree_io PRIVATE BTREE_ASYNC_MODE)
target_link_libraries(test_cow_btree_io PRIVATE
    hs_cow_btree
    hs_blob
    hs_checkpoint
    hs_meta
    hs_device
    hs_blkalloc
    hs_base
    iomanager
    sisl_cache
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME CowBtreeIOTest COMMAND test_cow_btree_io)
