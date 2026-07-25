cmake_minimum_required(VERSION 3.13)

include_directories (BEFORE ..)
include_directories (BEFORE .)

# ResourceMgr is the TOP layer: it starts after DeviceManager but is compiled against every layer below
# (device, logstore, replication) so it can measure footprint and drive truncation.
add_library(hs_resource OBJECT)
target_sources(hs_resource PRIVATE
        resource_mgr.cpp
      )
target_link_libraries(hs_resource ${COMMON_DEPS})
