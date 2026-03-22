include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_device OBJECT)
target_sources(hs_device PRIVATE
    chunk.cpp
    device_manager.cpp
    physical_dev.cpp
    virtual_dev.cpp
    journal_vdev.cpp
)
target_link_libraries(hs_device hs_common ${COMMON_DEPS})
