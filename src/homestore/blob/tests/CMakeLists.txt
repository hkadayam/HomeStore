include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(BLOB_STREAM_TEST_LIBS
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

add_executable(test_raw_blk_stream test_raw_blk_stream.cpp)
target_link_libraries(test_raw_blk_stream PRIVATE ${BLOB_STREAM_TEST_LIBS})
add_test(NAME RawBlkStream COMMAND test_raw_blk_stream)

add_executable(test_append_blk_stream test_append_blk_stream.cpp)
target_link_libraries(test_append_blk_stream PRIVATE ${BLOB_STREAM_TEST_LIBS})
add_test(NAME AppendBlkStream COMMAND test_append_blk_stream)

add_executable(test_append_byte_stream test_append_byte_stream.cpp)
target_link_libraries(test_append_byte_stream PRIVATE ${BLOB_STREAM_TEST_LIBS})
add_test(NAME AppendByteStream COMMAND test_append_byte_stream)
