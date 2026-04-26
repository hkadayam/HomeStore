cmake_minimum_required(VERSION 3.13)

# Include roots: <homestore/...> and <sisl/...> from src/, tests see homestore/index/btree/tests/ helpers.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../include)

add_executable(test_mem_btree test_mem_btree.cpp)
target_link_libraries(test_mem_btree PRIVATE
    hs_mem_btree
    iomanager
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME MemBtreeTest COMMAND test_mem_btree)

add_executable(test_btree_node ../../btree/tests/test_btree_node.cpp)
target_link_libraries(test_btree_node PRIVATE
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME BtreeNodeTest COMMAND test_btree_node)
