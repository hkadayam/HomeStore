include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hdb_common OBJECT)
target_sources(hdb_common PRIVATE
    kv_spec.cpp
)
target_compile_definitions(hdb_common PUBLIC BTREE_ASYNC_MODE)
target_link_libraries(hdb_common ${COMMON_DEPS})
