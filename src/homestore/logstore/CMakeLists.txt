include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_logstore OBJECT)
target_sources(hs_logstore PRIVATE
    log_stream.cpp
)
target_link_libraries(hs_logstore hs_blob ${COMMON_DEPS})
