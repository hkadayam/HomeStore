include_directories (BEFORE ..)
include_directories (BEFORE .)

add_library(hs_replication OBJECT)
target_sources(hs_replication PRIVATE
    repl_manager.cpp
    replica_set.cpp
    home_raft_log_store.cpp
    transport/folly_rpc_client.cpp
    transport/folly_rpc_client_factory.cpp
    transport/folly_rpc_listener.cpp
    transport/nuraft_codec.cpp
    )
target_link_libraries(hs_replication PRIVATE ${COMMON_DEPS})
# NuRaft (coro-mode fork) supplies the libnuraft/*.hxx headers this layer compiles against.  Linking the target
# propagates its INTERFACE include dir (extern/NuRaft/include) to the object-lib compilation.
if (TARGET NuRaft::static_lib)
    target_link_libraries(hs_replication PRIVATE NuRaft::static_lib)
endif()

add_subdirectory(tests)
