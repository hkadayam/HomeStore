add_executable(test_iomgr test_iomgr.cpp)
target_link_libraries(test_iomgr PRIVATE
    iomanager
    sisl_metrics
    sisl_logging
    GTest::gtest
)
add_test(NAME IOMgr COMMAND test_iomgr)

add_executable(test_drive test_drive.cpp)
target_link_libraries(test_drive PRIVATE
    iomanager
    sisl_metrics
    sisl_logging
    GTest::gtest
)
add_test(NAME Drive COMMAND test_drive)
