include_directories (BEFORE ..)
include_directories (BEFORE .)

add_library(hs_replication OBJECT)
target_sources(hs_replication PRIVATE
    repl_manager.cpp
    replica_set.cpp
    raft_state_machine.cpp
    home_raft_log_store.cpp
    common.cpp
    transport/folly_rpc_client.cpp
    transport/folly_rpc_client_factory.cpp
    transport/folly_rpc_listener.cpp
    )
target_link_libraries(hs_replication PRIVATE ${COMMON_DEPS} hs_common)
