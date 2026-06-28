#include "drive_interface.h"
#include "iomanager.h"
#include "common/defs.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <folly/futures/Promise.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>     // BLKGETSIZE64, BLKZEROOUT
#include <linux/falloc.h> // FALLOC_FL_ZERO_RANGE
#include <folly/experimental/io/IoUringBackend.h>
#endif

namespace iomanager {

#define DRIVE_LOG(level, dev, msg, ...) LOGTRACEMOD(iomgr, "[dev={}] " msg, (dev).dev_name, ##__VA_ARGS__)

// ── IoDevice ──────────────────────────────────────────────────────────────────

IoDevice::IoDevice(int fd_, std::string name, bool is_blk) noexcept :
        fd(fd_), dev_name(std::move(name)), is_block_device(is_blk) {
}

IoDevice::~IoDevice() {
    if (fd >= 0)
        ::close(fd);
}

// ── DriveInterface lifecycle ──────────────────────────────────────────────────

DriveInterface::DriveInterface() = default;
DriveInterface::~DriveInterface() = default;

// ─────────────────────────────────────────────────────────────────────────────
#ifdef __linux__
// ── Linux: io_uring path ─────────────────────────────────────────────────────
//
// Each reactor thread holds a DriveReactor with the IoUringBackend* and
// EventBase*.  The IoUringBackend drives itself — eb_event_base_loop() calls
// prepList() + processActiveEvents() every iteration.  New SQEs added by
// completion callbacks are picked up by the POLL_SQ kernel thread on the next
// iteration without a syscall.
//
// Off-reactor callers hop to a reactor by re-calling the same public method
// via spawn_waitable.  On the reactor t_dr is set, so the hop is skipped and
// the io_uring work executes inline.
// ─────────────────────────────────────────────────────────────────────────────

struct DriveReactor {
    folly::IoUringBackend* uring;
    folly::EventBase* eb;

    explicit DriveReactor(folly::EventBase* eb_) :
            uring(static_cast< folly::IoUringBackend* >(eb_->getBackend())), eb(eb_) {}
};

static thread_local DriveReactor* t_dr = nullptr;
static thread_local std::unique_ptr< DriveReactor > t_dr_owner;

void drive_interface_init_reactor(folly::EventBase* eb) {
    t_dr_owner = std::make_unique< DriveReactor >(eb);
    t_dr = t_dr_owner.get();
    LOGDEBUGMOD(iomgr, "DriveReactor init: eb={} uring={}", fmt::ptr(eb), fmt::ptr(t_dr->uring));

    eb->runOnDestruction([]() {
        LOGDEBUGMOD(iomgr, "DriveReactor cleanup: clearing t_dr");
        t_dr = nullptr;
        t_dr_owner.reset();
    });
}

static std::error_code to_ec(int res) {
    return res < 0 ? std::error_code(-res, std::generic_category()) : std::error_code{};
}

// ── Public API — Linux ────────────────────────────────────────────────────────

folly::coro::Task< std::shared_ptr< IoDevice > > DriveInterface::open_dev(std::string devname, int oflags) {
    int fd = ::open(devname.c_str(), oflags, 0666);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "open: " + devname);
    struct stat st {};
    ::fstat(fd, &st);
    co_return std::make_shared< IoDevice >(fd, std::move(devname), S_ISBLK(st.st_mode));
}

folly::coro::Task< uint64_t > DriveInterface::get_size(const IoDevice& dev) {
    if (dev.is_block_device) {
        uint64_t sz = 0;
        if (::ioctl(dev.fd, BLKGETSIZE64, &sz) < 0)
            throw std::system_error(errno, std::generic_category(), "BLKGETSIZE64");
        co_return sz;
    }
    struct stat st {};
    ::fstat(dev.fd, &st);
    co_return to_u64(st.st_size);
}

folly::coro::Task< std::error_code > DriveInterface::read(const IoDevice& dev, IoBuf& buf, uint64_t offset) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), read(dev, buf, offset));
    }
    DRIVE_LOG(TRACE, dev, "read: size={} offset={}", buf.size(), offset);
    folly::Promise< int > p;
    auto sf = p.getSemiFuture();
    // Direct-IO alignment fallback: io_uring requires aligned buffers.  If buf isn't aligned, alloc an
    // aligned temp, read into it, and copy out to buf after the IO completes.
    if (buf.is_aligned()) {
        t_dr->uring->queueRead(dev.fd, buf.bytes(), to_u32(buf.size()), (off_t)offset,
                               [p = std::move(p)](int res) mutable { p.setValue(res); });
        auto ec = to_ec(co_await std::move(sf).via(t_dr->eb));
        DRIVE_LOG(TRACE, dev, "read: size={} offset={} completed ec={}", buf.size(), offset, ec.message());
        co_return ec;
    } else {
        sisl::IoBufOwn temp{buf.size(), 512};
        t_dr->uring->queueRead(dev.fd, temp.bytes(), to_u32(buf.size()), (off_t)offset,
                               [p = std::move(p)](int res) mutable { p.setValue(res); });
        auto ec = to_ec(co_await std::move(sf).via(t_dr->eb));
        if (!ec) {
            std::memcpy(buf.bytes(), temp.cbytes(), buf.size());
        }
        DRIVE_LOG(TRACE, dev, "read[unaligned-fallback]: size={} offset={} completed ec={}", buf.size(), offset,
                  ec.message());
    }
    co_return ec;
}

folly::coro::Task< std::error_code > DriveInterface::write(const IoDevice& dev, const IoBuf& buf, uint64_t offset) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), write(dev, buf, offset));
    }
    DRIVE_LOG(TRACE, dev, "write: size={} offset={}", buf.size(), offset);
    folly::Promise< int > p;
    auto sf = p.getSemiFuture();
    if (buf.is_aligned()) {
        t_dr->uring->queueWrite(dev.fd, buf.cbytes(), to_u32(buf.size()), (off_t)offset,
                                [p = std::move(p)](int res) mutable { p.setValue(res); });
        auto ec = to_ec(co_await std::move(sf).via(t_dr->eb));
        DRIVE_LOG(TRACE, dev, "write: size={} offset={} completed ec={}", buf.size(), offset, ec.message());
        co_return ec;
    } else {
        sisl::IoBufOwn temp{buf.size(), 512};
        std::memcpy(temp.bytes(), buf.cbytes(), buf.size());
        t_dr->uring->queueWrite(dev.fd, temp.cbytes(), to_u32(buf.size()), (off_t)offset,
                                [p = std::move(p)](int res) mutable { p.setValue(res); });
        auto ec = to_ec(co_await std::move(sf).via(t_dr->eb));
        DRIVE_LOG(TRACE, dev, "write[unaligned-fallback]: size={} offset={} completed ec={}", buf.size(), offset,
                  ec.message());
    }
    co_return ec;
}

folly::coro::Task< std::error_code > DriveInterface::readv(const IoDevice& dev, sisl::SgList const& sg,
                                                            uint64_t offset) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), readv(dev, sg, offset));
    }
    folly::Promise< int > p;
    auto sf = p.getSemiFuture();

    // Per-element alignment check.  Aligned entries pass through with no copy.  Unaligned entries get a
    // same-indexed aligned temp; the iovec slot points at the temp; after the IO completes we copy each
    // temp back into its original buf.  aligned_temps / copy_back stay zero-alloc until the first unaligned
    // entry forces a reserve (common case is all-aligned — no allocation).
    std::vector< sisl::IoBufOwn > aligned_temps;
    std::vector< std::pair< sisl::IoBuf*, sisl::IoBufOwn* > > copy_back; // (dst, src) for unaligned
    std::vector< struct iovec > iovs;
    iovs.reserve(sg.bufs.size() + 1); // +1 for the optional tail pad
    uint64_t total = 0;
    for (auto* b : sg.bufs) {
        total += b->size();
        if (b->is_aligned()) {
            iovs.push_back({b->bytes(), b->size()});
        } else {
            if (aligned_temps.empty()) {
                aligned_temps.reserve(sg.bufs.size() + 1);
                copy_back.reserve(sg.bufs.size());
            }
            aligned_temps.emplace_back(b->size(), 512);
            iovs.push_back({aligned_temps.back().bytes(), b->size()});
            copy_back.emplace_back(b, &aligned_temps.back());
        }
    }
    // O_DIRECT requires block-multiple total bytes.  If the SgList's total isn't 512-multiple, append a
    // pad iov reading into a throwaway aligned temp — bytes are discarded (no copy_back entry).
    if (auto const rem = total % 512; rem != 0) {
        if (aligned_temps.empty()) { aligned_temps.reserve(1); }
        aligned_temps.emplace_back(to_u32(512 - rem), 512);
        iovs.push_back({aligned_temps.back().bytes(), 512 - rem});
    }
    t_dr->uring->queueReadv(dev.fd, {iovs.data(), iovs.data() + iovs.size()}, (off_t)offset,
                            [p = std::move(p)](int res) mutable { p.setValue(res); });
    auto ec = to_ec(co_await std::move(sf).via(t_dr->eb));
    if (!ec) {
        for (auto& [dst, src] : copy_back) {
            std::memcpy(dst->bytes(), src->cbytes(), src->size());
        }
    }
    co_return ec;
}

folly::coro::Task< std::error_code > DriveInterface::do_writev(const IoDevice& dev, std::vector< struct iovec >&& iovs,
                                                               uint64_t offset) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), do_writev(dev, std::move(iovs), offset));
    }
    folly::Promise< int > p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueWritev(dev.fd, {iovs.data(), iovs.data() + iovs.size()}, (off_t)offset,
                             [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return to_ec(co_await std::move(sf).via(t_dr->eb));
}

folly::coro::Task< std::error_code > DriveInterface::writev(const IoDevice& dev, sisl::SgList const& sg,
                                                              uint64_t offset) {
    // Per-element alignment check.  Aligned sources pass through with no copy.  Unaligned sources get a
    // same-indexed aligned temp (memcpy'd from the source); the iovec slot points at the temp.  Temps live
    // until do_writev's await completes.  aligned_temps stays zero-alloc until the first unaligned entry
    // forces a reserve (common case is all-aligned — no allocation).
    std::vector< sisl::IoBufOwn > aligned_temps;
    std::vector< struct iovec > iovs;
    iovs.reserve(sg.bufs.size() + 1); // +1 for the optional tail pad
    uint64_t total = 0;
    for (auto const* b : sg.bufs) {
        total += b->size();
        if (b->is_aligned()) {
            iovs.push_back({const_cast< uint8_t* >(b->cbytes()), b->size()});
        } else {
            if (aligned_temps.empty()) {
                aligned_temps.reserve(sg.bufs.size() + 1);
            }
            aligned_temps.emplace_back(b->size(), 512);
            std::memcpy(aligned_temps.back().bytes(), b->cbytes(), b->size());
            iovs.push_back({aligned_temps.back().bytes(), b->size()});
        }
    }
    // O_DIRECT requires block-multiple total bytes.  If the SgList's total isn't 512-multiple, append a
    // zero-filled pad iov so the kernel sees a valid IO size.  Pad bytes land on disk but are unread —
    // the on-disk record's value_size tells the reader how much to consume.
    if (auto const rem = total % 512; rem != 0) {
        if (aligned_temps.empty()) { aligned_temps.reserve(1); }
        aligned_temps.emplace_back(to_u32(512 - rem), 512);
        std::memset(aligned_temps.back().bytes(), 0, 512 - rem);
        iovs.push_back({aligned_temps.back().bytes(), 512 - rem});
    }
    // aligned_temps live in this coroutine frame — suspended through do_writev's await, so they outlive the IO.
    co_return co_await do_writev(dev, std::move(iovs), offset);
}

folly::coro::Task< std::error_code > DriveInterface::fsync(const IoDevice& dev) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), fsync(dev));
    }
    folly::Promise< int > p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueFdatasync(dev.fd, [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return to_ec(co_await std::move(sf).via(t_dr->eb));
}

folly::coro::Task< std::error_code > DriveInterface::write_zero(const IoDevice& dev, uint64_t size, uint64_t offset) {
    if (!t_dr) {
        co_return co_await iomgr().spawn_waitable(ReactorTarget::any(), write_zero(dev, size, offset));
    }
    int res;
    if (dev.is_block_device) {
        uint64_t range[2] = {offset, size};
        res = ::ioctl(dev.fd, BLKZEROOUT, range);
        if (res < 0)
            res = -errno;
    } else {
        folly::Promise< int > p;
        auto sf = p.getSemiFuture();
        t_dr->uring->queueFallocate(dev.fd, FALLOC_FL_ZERO_RANGE, (off_t)offset, (off_t)size,
                                    [p = std::move(p)](int res) mutable { p.setValue(res); });
        res = co_await std::move(sf).via(t_dr->eb);
    }
    co_return to_ec(res);
}

// ─────────────────────────────────────────────────────────────────────────────
#else // !__linux__
// ── Non-Linux: pread/pwrite on the global CPU thread pool ────────────────────
//
// The EventBase thread is never blocked. Each call hops to folly's CPU
// executor, runs the blocking syscall, then resumes on the original executor.
// ─────────────────────────────────────────────────────────────────────────────

void drive_interface_init_reactor(folly::EventBase*) {
}

folly::coro::Task< std::shared_ptr< IoDevice > > DriveInterface::open_dev(std::string devname, int oflags) {
    int fd = ::open(devname.c_str(), oflags, 0666);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(), "open: " + devname);
    struct stat st {};
    ::fstat(fd, &st);
    co_return std::make_shared< IoDevice >(fd, std::move(devname), false);
}

folly::coro::Task< uint64_t > DriveInterface::get_size(const IoDevice& dev) {
    struct stat st {};
    ::fstat(dev.fd, &st);
    co_return static_cast< uint64_t >(st.st_size);
}

folly::coro::Task< std::error_code > DriveInterface::read(const IoDevice& dev, IoBuf& buf, uint64_t offset) {
    int fd = dev.fd;
    void* ptr = buf.bytes();
    size_t sz = buf.size();
    co_return co_await folly::coro::co_invoke([fd, ptr, sz, offset]() -> folly::coro::Task< std::error_code > {
        ssize_t n = ::pread(fd, ptr, sz, (off_t)offset);
        co_return (n < 0) ? std::error_code(errno, std::generic_category()) : std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task< std::error_code > DriveInterface::write(const IoDevice& dev, const IoBuf& buf, uint64_t offset) {
    int fd = dev.fd;
    const void* ptr = buf.cbytes();
    size_t sz = buf.size();
    co_return co_await folly::coro::co_invoke([fd, ptr, sz, offset]() -> folly::coro::Task< std::error_code > {
        ssize_t n = ::pwrite(fd, ptr, sz, (off_t)offset);
        co_return (n < 0) ? std::error_code(errno, std::generic_category()) : std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task< std::error_code > DriveInterface::readv(const IoDevice& dev, sisl::SgList const& sg,
                                                            uint64_t offset) {
    int fd = dev.fd;
    auto const* bufs_ptr = &sg.bufs;
    co_return co_await folly::coro::co_invoke([fd, bufs_ptr, offset]() mutable -> folly::coro::Task< std::error_code > {
        for (auto* b : *bufs_ptr) {
            ssize_t n = ::pread(fd, b->bytes(), b->size(), (off_t)offset);
            if (n < 0) {
                co_return std::error_code(errno, std::generic_category());
            }
            offset += b->size();
        }
        co_return std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task< std::error_code > DriveInterface::do_writev(const IoDevice& dev, std::vector< struct iovec >&& iovs,
                                                               uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke([fd, iovs = std::move(iovs),
                                               offset]() mutable -> folly::coro::Task< std::error_code > {
        for (auto& iov : iovs) {
            ssize_t n = ::pwrite(fd, iov.iov_base, iov.iov_len, (off_t)offset);
            if (n < 0) {
                co_return std::error_code(errno, std::generic_category());
            }
            offset += iov.iov_len;
        }
        co_return std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task< std::error_code > DriveInterface::writev(const IoDevice& dev, sisl::SgList const& sg,
                                                              uint64_t offset) {
    std::vector< struct iovec > iovs;
    iovs.reserve(sg.bufs.size());
    for (auto const* b : sg.bufs) {
        iovs.push_back({const_cast< uint8_t* >(b->cbytes()), b->size()});
    }
    co_return co_await do_writev(dev, std::move(iovs), offset);
}

folly::coro::Task< std::error_code > DriveInterface::fsync(const IoDevice& dev) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke([fd]() -> folly::coro::Task< std::error_code > {
#ifdef __APPLE__
        // macOS lacks fdatasync; F_FULLFSYNC flushes write-back cache too
        int rc = ::fcntl(fd, F_FULLFSYNC);
#else
        int rc = ::fdatasync(fd);
#endif
        co_return (rc < 0) ? std::error_code(errno, std::generic_category()) : std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task< std::error_code > DriveInterface::write_zero(const IoDevice& dev, uint64_t size, uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke([fd, size, offset]() -> folly::coro::Task< std::error_code > {
        static constexpr size_t kChunk = 1u << 20; // 1 MiB
        IoBuf zbuf{static_cast< uint32_t >(std::min(size, (uint64_t)kChunk))};
        std::memset(zbuf.bytes(), 0, zbuf.size());
        uint64_t rem = size, off = offset;
        while (rem > 0) {
            size_t n = std::min(rem, (uint64_t)zbuf.size());
            if (::pwrite(fd, zbuf.cbytes(), n, (off_t)off) < 0)
                co_return std::error_code(errno, std::generic_category());
            off += n;
            rem -= n;
        }
        co_return std::error_code{};
    }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

#endif // __linux__

} // namespace iomanager
