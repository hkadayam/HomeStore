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
    find_package(liburing REQUIRED)
    # The conan-generated liburing package does not export its include directory,
    # so we add it explicitly via the package folder variable set by CMakeDeps.
    target_include_directories(iomanager PRIVATE "$<IF:$<CONFIG:Debug>,${liburing_PACKAGE_FOLDER_DEBUG},${liburing_PACKAGE_FOLDER_RELEASE}>/include")
    target_link_libraries(iomanager
        PUBLIC
            ${COMMON_DEPS}
            liburing::liburing
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
