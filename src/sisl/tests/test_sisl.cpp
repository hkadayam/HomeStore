#include <string>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

// ── Logging ────────────────────────────────────────────────────────────────────

TEST(Logging, MacrosDontCrash) {
    LOGINFO("info message: {}", 42);
    LOGWARN("warn message: {}", "hello");
    LOGERROR("error message");
    LOGDEBUG("debug message: {} {}", 1, 2);
}

TEST(Logging, SetAndGetModuleLevel) {
    sisl::logging::SetModuleLogLevel("base", spdlog::level::warn);
    EXPECT_EQ(sisl::logging::GetModuleLogLevel("base"), spdlog::level::warn);

    sisl::logging::SetModuleLogLevel("base", spdlog::level::info);
    EXPECT_EQ(sisl::logging::GetModuleLogLevel("base"), spdlog::level::info);
}

TEST(Logging, SetAllModuleLevels) {
    sisl::logging::SetAllModuleLogLevel(spdlog::level::debug);
    EXPECT_EQ(sisl::logging::GetModuleLogLevel("base"), spdlog::level::debug);

    // restore
    sisl::logging::SetAllModuleLogLevel(spdlog::level::info);
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    sisl::logging::SetLogger("test_sisl");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
