cmake_minimum_required(VERSION 3.13)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Multi-process replication test.  Links the aggregate `homestore` static lib (all module objects + NuRaft, which
# is PUBLIC on homestore).  Compiled in sync btree mode (no BTREE_ASYNC_MODE) so MemBtreeStore's NodeCore layout
# matches the sync-built hs_mem_btree library it links against — mixing modes is an ODR violation.
add_executable(test_replica_set test_replica_set.cpp)
target_link_libraries(test_replica_set PRIVATE
    homestore
    iomanager
    sisl_cache
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gmock
)
add_test(NAME ReplicaSet COMMAND test_replica_set)
