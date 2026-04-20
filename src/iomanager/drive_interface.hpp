#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <sisl/fds/buffer.h>

namespace iomanager {

using sisl::IOBuffer;

// Called once per reactor thread by IOManager::start(), before the loop runs.
// Sets up the per-reactor IoUringBackend handle and registers the loopPoll
// callback that drains CQEs at the start of every EventBase iteration.
void drive_interface_init_reactor(folly::EventBase* eb);

// ─────────────────────────────────────────────────────────────────────────────
// IoDevice — an open file or block-device file descriptor.
// The destructor closes the fd.  Use shared_ptr for shared ownership.
// ─────────────────────────────────────────────────────────────────────────────

struct IoDevice {
    int fd{-1};
    std::string dev_name;
    bool is_block_device{false};

    IoDevice(int fd, std::string name, bool is_blk) noexcept;
    ~IoDevice();

    IoDevice(const IoDevice&) = delete;
    IoDevice& operator=(const IoDevice&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// DriveInterface — stateless; all I/O methods are coroutines.
//
// On Linux  : async_read/write use io_uring (one ring per reactor thread).
// Non-Linux : async_read/write delegate to pread/pwrite on a CPU thread pool
//             so the EventBase thread is never blocked.
//
//   - read/readv  : buf(s) taken by value, returned together with the result
//   - write/writev: buf(s) passed by const-ref or value; caller must keep them
//                   alive across the co_await (guaranteed by coroutine frame)
// ─────────────────────────────────────────────────────────────────────────────

class DriveInterface {
public:
    DriveInterface();
    ~DriveInterface();

    DriveInterface(const DriveInterface&) = delete;
    DriveInterface& operator=(const DriveInterface&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Open a file or block device. `oflags`: e.g. O_RDWR | O_DIRECT.
    static folly::coro::Task< std::shared_ptr< IoDevice > > open_dev(std::string devname, int oflags);

    /// Returns device/file size in bytes.
    static folly::coro::Task< uint64_t > get_size(const IoDevice& dev);

    /// No-op kept for API symmetry; ownership is released by dropping the ptr.
    void close_dev(std::shared_ptr< IoDevice >) noexcept {}

    // ── Read ──────────────────────────────────────────────────────────────────

    /// Positioned read into buf. Caller retains ownership; buf must remain
    /// valid until the Task completes (guaranteed when caller co_awaits).
    folly::coro::Task< std::error_code > read(const IoDevice& dev, IOBuffer& buf, uint64_t offset);

    /// Scatter read into bufs. Same lifetime contract as read().
    folly::coro::Task< std::error_code > readv(const IoDevice& dev, std::vector< IOBuffer >& bufs, uint64_t offset);

    // ── Write ─────────────────────────────────────────────────────────────────

    /// Positioned write. `buf` must remain valid until the Task completes
    /// (guaranteed when the caller co_awaits the returned Task).
    folly::coro::Task< std::error_code > write(const IoDevice& dev, const IOBuffer& buf, uint64_t offset);

    /// Gather write. Buffers are read-only; caller retains ownership.
    folly::coro::Task< std::error_code > writev(const IoDevice& dev, const std::vector< IOBuffer >& bufs, uint64_t offset);
    folly::coro::Task< std::error_code > writev(const IoDevice& dev, const std::vector< sisl::ByteArray >& bufs, uint64_t offset);

    // ── Misc ──────────────────────────────────────────────────────────────────

    /// Zero [offset, offset+size).  Uses BLKZEROOUT on block devices (Linux).
    folly::coro::Task< std::error_code > write_zero(const IoDevice& dev, uint64_t size, uint64_t offset);

    /// fdatasync(2) — flush kernel buffers to backing storage.
    folly::coro::Task< std::error_code > fsync(const IoDevice& dev);

private:
    folly::coro::Task< std::error_code > do_writev(const IoDevice& dev, std::vector< struct iovec >&& iovs,
                                                    uint64_t offset);
};

} // namespace iomanager
