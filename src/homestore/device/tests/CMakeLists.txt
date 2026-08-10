include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_pdev test_pdev.cpp)
target_link_libraries(test_pdev PRIVATE
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
add_test(NAME PDevTest COMMAND test_pdev)

add_executable(test_vdev test_vdev.cpp)
target_link_libraries(test_vdev PRIVATE
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
add_test(NAME VDevTest COMMAND test_vdev)

add_executable(test_dev_mgr test_dev_mgr.cpp)
target_link_libraries(test_dev_mgr PRIVATE
    hs_device
    hs_blkalloc
    hs_base
    iomanager
    sisl_metrics
    sisl_settings
    sisl_logging
    sisl_flip
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME DevMgrTest COMMAND test_dev_mgr)
