cmake_minimum_required(VERSION 3.11)

include_directories (BEFORE ../)

add_library(hs_cow_btree OBJECT)
target_sources(hs_cow_btree PRIVATE
    cow_btree_mgr.cpp
    cow_btree.cpp
    )
target_link_libraries(hs_cow_btree hs_common ${COMMON_DEPS})
