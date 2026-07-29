# Vendored sisl components
# These were previously pulled in via the external sisl conan package.
# Each compiled component is a separate OBJECT library so the top-level
# homestore static library can selectively link what it needs.

# ── cache ─────────────────────────────────────────────────────────────────────
add_library(sisl_cache OBJECT
    cache/two_q_evictor.cpp
    cache/lru_evictor.cpp
)
target_link_libraries(sisl_cache PUBLIC
    sisl_logging
    ${COMMON_DEPS}
    Snappy::snappy
    userspace-rcu::userspace-rcu
)

# ── logging ───────────────────────────────────────────────────────────────────
add_library(sisl_logging OBJECT
    logging/logging.cpp
)
target_link_libraries(sisl_logging PUBLIC
    ${COMMON_DEPS}
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# ── metrics ───────────────────────────────────────────────────────────────────
add_library(sisl_metrics OBJECT
    metrics/metrics.cpp
    metrics/metrics_group_impl.cpp
    metrics/metrics_atomic.cpp
    metrics/metrics_folly.cpp
)
target_link_libraries(sisl_metrics PUBLIC
    sisl_logging
    ${COMMON_DEPS}
)

# ── settings ──────────────────────────────────────────────────────────────────
add_library(sisl_settings OBJECT
    settings/settings.cpp
)
target_link_libraries(sisl_settings PUBLIC
    sisl_logging
    ${COMMON_DEPS}
    flatbuffers::flatbuffers
    userspace-rcu::userspace-rcu
)

# ── version ───────────────────────────────────────────────────────────────────
add_library(sisl_version OBJECT
    version.cpp
)
target_link_libraries(sisl_version PUBLIC
    ${COMMON_DEPS}
)

add_subdirectory(flip)

if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_subdirectory(logging/tests)
    add_subdirectory(cache/tests)
    add_subdirectory(fds/tests)
    add_subdirectory(metrics/tests)
    add_subdirectory(settings/tests)
endif()
