#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __linux__
#  include <fcntl.h>
#else
#  include <fcntl.h>
#endif

#include <gtest/gtest.h>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <iomanager/iomanager.h>
#include <iomanager/drive_interface.hpp>

using namespace iomanager;
using namespace std::chrono_literals;

SISL_OPTION_GROUP(test_drive,
    (num_reactors, "", "num_reactors", "reactor thread count",
     ::cxxopts::value<uint32_t>()->default_value("4"), "number"),
    (num_ios, "", "num_ios", "IO operations per reactor",
     ::cxxopts::value<uint32_t>()->default_value("1000"), "number"),
    (dev_path, "", "dev_path", "path for the test file",
     ::cxxopts::value<std::string>()->default_value("/tmp/hs_test_drive"), "path"),
    (dev_size_mb, "", "dev_size_mb", "file size in MB",
     ::cxxopts::value<uint32_t>()->default_value("64"), "number"))

static uint32_t g_num_reactors{4};
static uint32_t g_num_ios{1000};
static std::string g_dev_path{"/tmp/hs_test_drive"};
static uint64_t g_dev_size{64 * 1024 * 1024};

static constexpr size_t kBlockSize{4096};

// Fill buf with the pattern: every sizeof(uint64_t) bytes = offset.
static void fill_pattern(IOBuffer& buf, uint64_t offset) {
    auto* p = reinterpret_cast<uint64_t*>(buf.bytes());
    for (size_t i = 0; i < buf.size() / sizeof(uint64_t); ++i) {
        p[i] = offset;
    }
}

static bool verify_pattern(const IOBuffer& buf, uint64_t offset) {
    const auto* p = reinterpret_cast<const uint64_t*>(buf.cbytes());
    for (size_t i = 0; i < buf.size() / sizeof(uint64_t); ++i) {
        if (p[i] != offset) return false;
    }
    return true;
}

class DriveTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_iomgr(g_num_reactors);

        // Create the backing file if it doesn't exist.
        const std::filesystem::path p{g_dev_path};
        if (!std::filesystem::exists(p)) {
            auto fd = ::open(g_dev_path.c_str(), O_RDWR | O_CREAT, 0666);
            ASSERT_GT(fd, 0) << "failed to create test file";
            ::close(fd);
            std::filesystem::resize_file(p, g_dev_size);
            m_created = true;
        }

        // Open device from reactor 0.
        m_iodev = iomgr().spawn_and_block(
            ReactorTarget::reactor(0),
            DriveInterface::open_dev(g_dev_path, O_RDWR));
        ASSERT_NE(m_iodev, nullptr);
    }

    void TearDown() override {
        m_iodev.reset();
        stop_iomgr();
        if (m_created) {
            std::filesystem::remove(std::filesystem::path{g_dev_path});
        }
    }

    std::shared_ptr<IoDevice> m_iodev;
    bool m_created{false};
};

// ── Single-reactor write → read → verify ──────────────────────────────────────
//
// Old: issue_preload() / do_verify() on a single worker thread
// New: co_await drive_.write() / drive_.read() inside a coroutine

TEST_F(DriveTest, SingleReactorWriteReadVerify) {
    DriveInterface drive;
    const size_t num_blocks = 16;

    iomgr().spawn_and_block(ReactorTarget::reactor(0),
        [this, &drive, num_blocks]() -> folly::coro::Task<void> {
            for (size_t i = 0; i < num_blocks; ++i) {
                const uint64_t offset = i * kBlockSize;

                // Write
                IOBuffer wbuf{static_cast<uint32_t>(kBlockSize)};
                fill_pattern(wbuf, offset);
                auto wec = co_await drive.write(*m_iodev, wbuf, offset);
                EXPECT_FALSE(wec) << "write failed at offset " << offset << ": " << wec.message();

                // Read back
                IOBuffer rbuf{static_cast<uint32_t>(kBlockSize)};
                auto rec = co_await drive.read(*m_iodev, rbuf, offset);
                EXPECT_FALSE(rec) << "read failed at offset " << offset << ": " << rec.message();
                EXPECT_TRUE(verify_pattern(rbuf, offset)) << "data mismatch at offset " << offset;
            }
        }());
}

// ── Multi-reactor concurrent IO ────────────────────────────────────────────────
//
// Old: io_on_worker_threads() — each thread owns a range, preloads, then R/W
// New: spawn_waitable_all — each reactor owns its slice, runs the same pattern

TEST_F(DriveTest, MultiReactorConcurrentWriteReadVerify) {
    DriveInterface drive;

    // Slice the file: each reactor gets an equal range.
    const uint64_t per_reactor = g_dev_size / g_num_reactors;
    const uint32_t ios_per_reactor = g_num_ios / g_num_reactors;

    auto results = iomgr().spawn_and_block(
        ReactorTarget::reactor(0),
        iomgr().spawn_waitable_all(
            [this, &drive, per_reactor, ios_per_reactor](size_t reactor_id)
                -> folly::coro::Task<uint32_t>
            {
                const uint64_t region_start = reactor_id * per_reactor;
                const uint64_t region_end   = region_start + per_reactor;
                uint32_t completed{0};

                for (uint32_t i = 0; i < ios_per_reactor; ++i) {
                    // Simple sequential pattern within this reactor's region.
                    const uint64_t offset = region_start
                        + (static_cast<uint64_t>(i) * kBlockSize) % per_reactor;
                    // round down to block boundary
                    const uint64_t aligned = (offset / kBlockSize) * kBlockSize;
                    if (aligned + kBlockSize > region_end) continue;

                    // Write
                    IOBuffer wbuf{static_cast<uint32_t>(kBlockSize)};
                    fill_pattern(wbuf, aligned);
                    auto wec = co_await drive.write(*m_iodev, wbuf, aligned);
                    EXPECT_FALSE(wec) << "write error: " << wec.message();

                    // Read back
                    IOBuffer rbuf{static_cast<uint32_t>(kBlockSize)};
                    auto rec = co_await drive.read(*m_iodev, rbuf, aligned);
                    EXPECT_FALSE(rec) << "read error: " << rec.message();
                    EXPECT_TRUE(verify_pattern(rbuf, aligned)) << "mismatch at " << aligned;

                    ++completed;
                }
                co_return completed;
            }));

    ASSERT_EQ(results.size(), g_num_reactors);
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_GT(results[i], 0u) << "reactor " << i << " completed no IOs";
        LOGINFO("Reactor {}: {} IOs completed", i, results[i]);
    }
}

// ── WriteZero ─────────────────────────────────────────────────────────────────
//
// Old: test_write_zero.cpp
// New: drive.write_zero() + verify region reads back as zeros

TEST_F(DriveTest, WriteZeroThenVerify) {
    DriveInterface drive;
    const uint64_t offset = 0;
    const uint64_t size   = 4 * kBlockSize;

    iomgr().spawn_and_block(ReactorTarget::reactor(0),
        [this, &drive, offset, size]() -> folly::coro::Task<void> {
            // First write a non-zero pattern.
            for (uint64_t off = offset; off < offset + size; off += kBlockSize) {
                IOBuffer wbuf{static_cast<uint32_t>(kBlockSize)};
                fill_pattern(wbuf, off + 1);  // non-zero
                co_await drive.write(*m_iodev, wbuf, off);
            }

            // Zero the region.
            auto ec = co_await drive.write_zero(*m_iodev, size, offset);
            EXPECT_FALSE(ec) << "write_zero failed: " << ec.message();

            // Verify zeros.
            for (uint64_t off = offset; off < offset + size; off += kBlockSize) {
                IOBuffer rbuf{static_cast<uint32_t>(kBlockSize)};
                auto rec = co_await drive.read(*m_iodev, rbuf, off);
                EXPECT_FALSE(rec) << "read failed: " << rec.message();
                const auto* p = reinterpret_cast<const uint64_t*>(rbuf.cbytes());
                for (size_t i = 0; i < rbuf.size() / sizeof(uint64_t); ++i) {
                    EXPECT_EQ(p[i], 0u) << "non-zero at offset " << off << " word " << i;
                }
            }
        }());
}

// ── Fsync ──────────────────────────────────────────────────────────────────────

TEST_F(DriveTest, FsyncAfterWrite) {
    DriveInterface drive;

    iomgr().spawn_and_block(ReactorTarget::reactor(0),
        [this, &drive]() -> folly::coro::Task<void> {
            IOBuffer wbuf{static_cast<uint32_t>(kBlockSize)};
            fill_pattern(wbuf, 0);
            co_await drive.write(*m_iodev, wbuf, 0);
            auto ec = co_await drive.fsync(*m_iodev);
            EXPECT_FALSE(ec) << "fsync failed: " << ec.message();
        }());
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv);
    g_num_reactors = SISL_OPTIONS["num_reactors"].as<uint32_t>();
    g_num_ios      = SISL_OPTIONS["num_ios"].as<uint32_t>();
    g_dev_path     = SISL_OPTIONS["dev_path"].as<std::string>();
    g_dev_size     = static_cast<uint64_t>(SISL_OPTIONS["dev_size_mb"].as<uint32_t>()) * 1024 * 1024;
    sisl::logging::SetLogger("test_drive");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
