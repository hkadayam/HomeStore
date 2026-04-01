include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_cp_mgr
    test_cp_mgr.cpp
)
target_link_libraries(test_cp_mgr PRIVATE
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
add_test(NAME CPMgrTest COMMAND test_cp_mgr)
