include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_blkalloc test_blkalloc.cpp)
target_link_libraries(test_blkalloc PRIVATE
    hs_blkalloc
    hs_base
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME BlkAllocTest COMMAND test_blkalloc)
