include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hdb_db_table OBJECT)
target_sources(hdb_db_table PRIVATE
    table.cpp
    db.cpp
    catalog.cpp
)
target_compile_definitions(hdb_db_table PUBLIC BTREE_ASYNC_MODE)
target_link_libraries(hdb_db_table hdb_wal hdb_index hdb_common ${COMMON_DEPS})
