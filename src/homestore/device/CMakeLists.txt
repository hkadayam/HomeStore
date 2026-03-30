include_directories(BEFORE ..)
include_directories(BEFORE .)

add_library(hs_device OBJECT)
target_sources(hs_device PRIVATE
    chunk.cpp
    device_manager.cpp
    physical_dev.cpp
    virtual_dev.cpp
)
target_link_libraries(hs_device ${COMMON_DEPS})

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
