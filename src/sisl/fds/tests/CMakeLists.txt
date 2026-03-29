set(FDS_TEST_DEPS
    sisl_logging
    sisl_metrics
    ${COMMON_DEPS}
    GTest::gtest
)

add_executable(test_bitword test_bitword.cpp)
target_link_libraries(test_bitword PRIVATE ${FDS_TEST_DEPS})
add_test(NAME Bitword COMMAND test_bitword)

add_executable(test_bitset test_bitset.cpp)
target_link_libraries(test_bitset PRIVATE ${FDS_TEST_DEPS})
add_test(NAME Bitset COMMAND test_bitset)

add_executable(test_atomic_status_counter test_atomic_status_counter.cpp)
target_link_libraries(test_atomic_status_counter PRIVATE ${FDS_TEST_DEPS})
add_test(NAME AtomicStatusCounter COMMAND test_atomic_status_counter)

add_executable(test_compact_bitset test_compact_bitset.cpp)
target_link_libraries(test_compact_bitset PRIVATE ${FDS_TEST_DEPS})
add_test(NAME CompactBitset COMMAND test_compact_bitset)

add_executable(test_concurrent_insert_vector test_concurrent_insert_vector.cpp)
target_link_libraries(test_concurrent_insert_vector PRIVATE ${FDS_TEST_DEPS})
add_test(NAME ConcurrentInsertVector COMMAND test_concurrent_insert_vector)

add_executable(test_stream_tracker test_stream_tracker.cpp)
target_link_libraries(test_stream_tracker PRIVATE sisl_metrics ${FDS_TEST_DEPS})
add_test(NAME StreamTracker COMMAND test_stream_tracker)