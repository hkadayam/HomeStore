add_executable(test_sisl test_sisl.cpp)
target_link_libraries(test_sisl PRIVATE
    sisl_logging    # pulls in logging .o + spdlog, nlohmann_json
    GTest::gtest
)
add_test(NAME Sisl COMMAND test_sisl)
