set(FLATBUFFERS_FLATC_EXECUTABLE ${flatbuffers_INCLUDE_DIR}/../bin/flatc)

add_executable(test_settings)
target_sources(test_settings PRIVATE
    test_settings.cpp
)
settings_gen_cpp(
    ${FLATBUFFERS_FLATC_EXECUTABLE}
    ${CMAKE_CURRENT_BINARY_DIR}/generated/
    test_settings
    test_app_schema.fbs
)
if(NOT ${CMAKE_CURRENT_BINARY_DIR} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(test_settings BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()
target_include_directories(test_settings BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_settings PRIVATE
    sisl_settings
    sisl_logging
    ${COMMON_DEPS}
    GTest::gtest
)
add_test(NAME Settings COMMAND test_settings)