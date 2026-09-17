# HomeDB — persistent, replicated, transactional KV built on HomeStore.  Top-level sibling to homestore.
#
# Structure mirrors homestore: each subdirectory produces one OBJECT library (hdb_<module>); this file
# aggregates them plus the top-level orchestrator sources into the STATIC library `homedb`.

include_directories(BEFORE ..)
include_directories(BEFORE .)

add_subdirectory(common)
add_subdirectory(index)
add_subdirectory(wal)
add_subdirectory(db_table)

set(HOMEDB_OBJECTS
    $<TARGET_OBJECTS:hdb_common>
    $<TARGET_OBJECTS:hdb_index>
    $<TARGET_OBJECTS:hdb_wal>
    $<TARGET_OBJECTS:hdb_db_table>
    home_db.cpp
)

add_library(homedb STATIC ${HOMEDB_OBJECTS})
target_compile_definitions(homedb PUBLIC BTREE_ASYNC_MODE)
target_link_libraries(homedb PUBLIC
    homestore
    iomanager
    ${COMMON_DEPS}
)

add_subdirectory(tests)
