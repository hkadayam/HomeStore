include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hdb_index OBJECT)
target_sources(hdb_index PRIVATE
    db_kv.cpp
    unsharded_btree.cpp
)
target_compile_definitions(hdb_index PUBLIC BTREE_ASYNC_MODE)
target_link_libraries(hdb_index hdb_common ${COMMON_DEPS})
