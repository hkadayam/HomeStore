cmake_minimum_required(VERSION 3.13)

include_directories (BEFORE ..)
include_directories (BEFORE .)

add_library(hs_blkalloc OBJECT)
target_sources(hs_blkalloc PRIVATE
        blk.cpp
        bitmap_blk_allocator.cpp
        slab_blk_allocator.cpp
        segment_manager.cpp
      )
target_link_libraries(hs_blkalloc ${COMMON_DEPS})

add_subdirectory(tests)
