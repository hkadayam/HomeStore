add_executable(test_logging test_logging.cpp)
target_link_libraries(test_logging PRIVATE
    sisl_logging
    GTest::gtest
)
add_test(NAME Logging COMMAND test_logging)