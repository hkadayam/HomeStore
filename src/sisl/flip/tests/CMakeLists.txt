cmake_minimum_required(VERSION 3.13)

# Ports of the old sisl flip self-tests (workspace_sds/sisl/src/flip/lib/test_flip.cpp and
# workspace_sds/sisl/src/flip/client/local/test_flip_local_client.cpp), rewritten to use the new flatbuffer-based
# FlipSpecT/FlipConditionT/FlipFrequencyT object-API instead of the old protobuf types.

add_executable(test_flip test_flip.cpp)
target_link_libraries(test_flip PRIVATE
    sisl_flip
    sisl_logging
    sisl_metrics
    sisl_settings
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME FlipTest COMMAND test_flip)

add_executable(test_flip_local_client test_flip_local_client.cpp)
target_link_libraries(test_flip_local_client PRIVATE
    sisl_flip
    sisl_logging
    sisl_metrics
    sisl_settings
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME FlipLocalClientTest COMMAND test_flip_local_client)