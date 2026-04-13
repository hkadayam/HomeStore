set(CACHE_TEST_DEPS
    sisl_cache
    sisl_logging
    ${COMMON_DEPS}
    Snappy::snappy
    userspace-rcu::userspace-rcu
    GTest::gtest
)

add_executable(test_simple_hashmap test_simple_hashmap.cpp)
target_link_libraries(test_simple_hashmap PRIVATE ${CACHE_TEST_DEPS})
add_test(NAME SimpleHashMap COMMAND test_simple_hashmap)

# test_lru_evictor disabled while LRUEvictor is being refactored.
# add_executable(test_lru_evictor test_lru_evictor.cpp)
# target_link_libraries(test_lru_evictor PRIVATE ${CACHE_TEST_DEPS})
# add_test(NAME LRUEvictor COMMAND test_lru_evictor)

add_executable(test_two_q_evictor test_two_q_evictor.cpp)
target_link_libraries(test_two_q_evictor PRIVATE ${CACHE_TEST_DEPS})
add_test(NAME TwoQEvictor COMMAND test_two_q_evictor)

add_executable(test_cache test_cache.cpp)
target_link_libraries(test_cache PRIVATE ${CACHE_TEST_DEPS})
add_test(NAME Cache COMMAND test_cache)