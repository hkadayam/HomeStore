include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(LOGSTREAM_TEST_LIBS
    hs_logstore
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

add_executable(test_log_stream test_log_stream.cpp)
target_link_libraries(test_log_stream PRIVATE ${LOGSTREAM_TEST_LIBS})
add_test(NAME LogStream COMMAND test_log_stream)

add_executable(test_log_store test_log_store.cpp)
target_link_libraries(test_log_store PRIVATE ${LOGSTREAM_TEST_LIBS})
add_test(NAME LogStore COMMAND test_log_store)

add_executable(test_log_store_mgr test_log_store_mgr.cpp)
target_link_libraries(test_log_store_mgr PRIVATE ${LOGSTREAM_TEST_LIBS})
add_test(NAME LogStoreManager COMMAND test_log_store_mgr)