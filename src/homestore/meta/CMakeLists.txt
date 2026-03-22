include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_meta OBJECT)
target_sources(hs_meta PRIVATE
    meta_blk.cpp
    meta_blk_manager.cpp
    meta_client.cpp
)
target_link_libraries(hs_meta hs_device hs_common ${COMMON_DEPS})
