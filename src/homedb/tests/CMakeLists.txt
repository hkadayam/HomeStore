include_directories(BEFORE ../..)
include_directories(BEFORE ..)

set(HOMEDB_TEST_LIBS
    homedb
    homestore
    iomanager
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    ${COMMON_TEST_DEPS}
    GTest::gtest
)

add_executable(test_homedb_basic test_homedb_basic.cpp)
target_compile_definitions(test_homedb_basic PRIVATE BTREE_ASYNC_MODE)
target_link_libraries(test_homedb_basic PRIVATE ${HOMEDB_TEST_LIBS})
add_test(NAME HomeDBBasic COMMAND test_homedb_basic)
