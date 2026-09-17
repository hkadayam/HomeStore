include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hdb_wal OBJECT)
target_sources(hdb_wal PRIVATE
    journal.cpp
    local_journal.cpp
    repl_journal.cpp
)
target_compile_definitions(hdb_wal PUBLIC BTREE_ASYNC_MODE)
target_link_libraries(hdb_wal hdb_common ${COMMON_DEPS})
