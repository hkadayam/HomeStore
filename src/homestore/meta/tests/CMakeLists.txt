include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_meta_blk_mgr
    test_meta_blk_mgr.cpp
)
target_link_libraries(test_meta_blk_mgr PRIVATE
    hs_meta
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
add_test(NAME MetaBlkMgrTest COMMAND test_meta_blk_mgr)