add_library(iomanager OBJECT
    iomanager.cpp
    drive_interface.cpp
)

target_include_directories(iomanager
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# On Linux link liburing for the io_uring backend.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(LIBURING_LIB uring REQUIRED)
    target_link_libraries(iomanager
        PUBLIC
            ${COMMON_DEPS}
            ${LIBURING_LIB}
    )
    target_compile_definitions(iomanager PUBLIC HS_HAVE_URING=1)
else()
    target_link_libraries(iomanager
        PUBLIC
            ${COMMON_DEPS}
    )
endif()

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()
