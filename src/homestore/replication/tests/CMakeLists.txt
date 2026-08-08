cmake_minimum_required(VERSION 3.13)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Multi-process replication test.  Links the aggregate `homestore` static lib (all module objects + NuRaft, which
# is PUBLIC on homestore).  Compiled in BTREE_ASYNC_MODE so the durable COWBtree-backed TestStore (the only TU that
# instantiates the btree templates, hs_test_cow_store.cpp) matches the async-built hs_cow_btree library.
#
# The whole suite is ONE binary (paying the slow btree compile once, in hs_test_cow_store.cpp) but split by test
# category across sibling files for readability: test_replica_set.cpp holds main() only, and each
# test_repl_<category>.cpp adds that category's cases against the shared fixture.
add_executable(test_replica_set
    test_replica_set.cpp
    test_repl_writes.cpp
    test_repl_reboot.cpp
    test_repl_crash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test_common/hs_test_cow_store.cpp)
target_compile_definitions(test_replica_set PRIVATE BTREE_ASYNC_MODE)
target_link_libraries(test_replica_set PRIVATE
    homestore
    iomanager
    sisl_flip
    sisl_cache
    sisl_metrics
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gmock
)
add_test(NAME ReplicaSet COMMAND test_replica_set)
