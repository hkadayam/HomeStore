cmake_minimum_required(VERSION 3.11)

include_directories (BEFORE ../)

add_subdirectory(mem_btree)
add_subdirectory(cow_btree)
