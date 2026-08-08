include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_blob OBJECT)
target_sources(hs_blob PRIVATE
    blk_read_tracker.cpp
    stream_base.cpp
    raw_blk_stream.cpp
    append_blk_stream.cpp
    append_byte_stream.cpp
    blob_dev.cpp
    blob_dev_mgr.cpp
)
target_link_libraries(hs_blob PRIVATE sisl_flip hs_device hs_meta hs_checkpoint ${COMMON_DEPS})

add_subdirectory(tests)
