include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_raw_blk_stream
    test_raw_blk_stream.cpp
)
target_link_libraries(test_raw_blk_stream PRIVATE
    hs_blob
    hs_checkpoint
    hs_meta
    hs_device
    hs_blkalloc
    hs_base
    iomanager
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME RawBlkStream COMMAND test_raw_blk_stream)
