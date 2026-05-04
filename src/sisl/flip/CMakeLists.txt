cmake_minimum_required(VERSION 3.13)

# sisl_flip — local-only fault injection API.  Header-only at the C++ level (flip.h, flip_client.h), but depends
# on a flatbuffer-generated header (flip_spec_generated.h) for the FlipSpec / FlipFrequency / FlipCondition types.
# The optional grpc-based remote-control rpc server (flip_rpc_server.h/.cpp) is NOT compiled here — bring grpc in
# and re-enable separately when the rpc surface is needed.

find_package(flatbuffers REQUIRED)
set(FLATBUFFERS_FLATC_EXECUTABLE ${flatbuffers_INCLUDE_DIR}/../bin/flatc)

set(FLIP_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(FLIP_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated/proto)
set(FLIP_GEN_HDR  ${FLIP_GEN_DIR}/flip_spec_generated.h)

file(MAKE_DIRECTORY ${FLIP_GEN_DIR})

add_custom_command(
    OUTPUT  ${FLIP_GEN_HDR}
    COMMAND ${FLATBUFFERS_FLATC_EXECUTABLE}
            -c -o ${FLIP_GEN_DIR}
            --no-prefix
            --scoped-enums
            --gen-mutable
            --gen-object-api
            --gen-name-strings
            ${FLIP_PROTO_DIR}/flip_spec.fbs
    DEPENDS ${FLIP_PROTO_DIR}/flip_spec.fbs
    COMMENT "flatc: generating ${FLIP_GEN_HDR}"
    VERBATIM
)

# Header-only library; the custom target carries the build-time dependency on the generated header so that any
# consumer linking sisl_flip will trigger the flatc step before its own compile.
add_custom_target(sisl_flip_gen DEPENDS ${FLIP_GEN_HDR})

add_library(sisl_flip INTERFACE)
add_dependencies(sisl_flip sisl_flip_gen)
target_include_directories(sisl_flip INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/generated      # for "proto/flip_spec_generated.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/..             # for <sisl/flip/...>
)
target_link_libraries(sisl_flip INTERFACE flatbuffers::flatbuffers)

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(tests)
endif()