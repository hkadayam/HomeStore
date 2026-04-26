cmake_minimum_required(VERSION 3.11)

include_directories (BEFORE ../../)

add_library(hs_mem_btree OBJECT)
target_sources(hs_mem_btree PRIVATE
    mem_btree.cpp
    mem_btree_drainer.cpp
    ../btree/btree_base.cpp
    )
# btree_base.cpp is compiled here (sync mode) so BtreeSharedMutex/NodeCore layouts match this TU's async-mode
# setting (unset).  The async backend (hs_cow_btree) compiles its own copy with BTREE_ASYNC_MODE — keeping a
# single hs_btree library would cause an ODR mismatch across sync and async consumers.
target_link_libraries(hs_mem_btree ${COMMON_DEPS})

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
