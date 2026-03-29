set(METRICS_TEST_DEPS
    sisl_metrics
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)

add_executable(test_metrics test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE ${METRICS_TEST_DEPS})
add_test(NAME Metrics COMMAND test_metrics)

add_executable(test_metrics_farm test_metrics_farm.cpp)
target_link_libraries(test_metrics_farm PRIVATE ${METRICS_TEST_DEPS})
add_test(NAME MetricsFarm COMMAND test_metrics_farm)