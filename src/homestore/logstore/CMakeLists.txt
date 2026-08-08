include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_logstore OBJECT)
target_sources(hs_logstore PRIVATE
    log_stream.cpp
    log_store.cpp
    log_store_mgr.cpp
)
target_link_libraries(hs_logstore PRIVATE sisl_flip hs_blob ${COMMON_DEPS})

add_subdirectory(tests)
