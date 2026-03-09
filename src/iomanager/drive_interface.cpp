#include "drive_interface.hpp"
#include "iomanager.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <folly/futures/Promise.h>

#ifdef __linux__
#  include <sys/ioctl.h>
#  include <linux/fs.h>       // BLKGETSIZE64, BLKZEROOUT
#  include <linux/falloc.h>   // FALLOC_FL_ZERO_RANGE
#  include <folly/io/async/IoUringBackend.h>
#endif

namespace homestore {

// ── IOBuffer ──────────────────────────────────────────────────────────────────

IOBuffer::IOBuffer(size_t size) : size_(align_up(size)) {
    data_ = static_cast<uint8_t*>(std::aligned_alloc(kAlign, size_));
    if (!data_) throw std::bad_alloc{};
}

IOBuffer::~IOBuffer() { std::free(data_); }

IOBuffer::IOBuffer(IOBuffer&& o) noexcept : data_(o.data_), size_(o.size_) {
    o.data_ = nullptr;
    o.size_ = 0;
}

IOBuffer& IOBuffer::operator=(IOBuffer&& o) noexcept {
    if (this != &o) {
        std::free(data_);
        data_   = o.data_;
        size_   = o.size_;
        o.data_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

// ── IoDevice ──────────────────────────────────────────────────────────────────

IoDevice::IoDevice(int fd_, std::string name, bool is_blk) noexcept
    : fd(fd_), dev_name(std::move(name)), is_block_device(is_blk) {}

IoDevice::~IoDevice() { if (fd >= 0) ::close(fd); }

// ── DriveInterface lifecycle ──────────────────────────────────────────────────

DriveInterface::DriveInterface() = default;
DriveInterface::~DriveInterface() = default;

// ─────────────────────────────────────────────────────────────────────────────
#ifdef __linux__
// ── Linux: io_uring path ─────────────────────────────────────────────────────
//
// Loop flow per EventBase iteration (POLL_SQ only, no POLL_CQ):
//
//   [runBeforeLoop] DriveReactor::runLoopCallback()
//     → loopPoll(): non-blocking CQ peek (zero syscall if ring is empty)
//     → completions fire FileOpCallback → Promise fulfilled
//     → coroutine continuations scheduled into loopCallbacks_
//     → re-arms itself for the next iteration
//
//   EventBase checks loopCallbacks_.empty()?
//     NO  → eb_event_base_loop(NONBLOCK) → one more non-blocking peek
//     YES → eb_event_base_loop(BLOCK)    → io_uring_enter(wait=1) → sleep
//           woken by: IO CQE | cross-thread notify-fd CQE | timer CQE
//
//   [runInLoop] coroutine continuations execute
//     → may call queueRead/Write → new SQEs enter submitList_
//     → POLL_SQ kernel thread picks them up without a submit syscall
//     → loop repeats
// ─────────────────────────────────────────────────────────────────────────────

struct DriveReactor : folly::EventBase::LoopCallback {
    folly::EventBase*      eb;
    folly::IoUringBackend* uring;

    explicit DriveReactor(folly::EventBase* eb_)
        : eb(eb_)
        , uring(static_cast<folly::IoUringBackend*>(eb_->getBackend())) {
        eb->runBeforeLoop(this);
    }

    void runLoopCallback() noexcept override {
        // prepList + non-blocking CQ peek + processActiveEvents.
        // Zero syscall when the ring is empty.
        // Any CQEs fulfilled here schedule continuations, which causes
        // the EventBase to use DONT_WAIT this iteration instead of sleeping.
        uring->loopPoll();
        eb->runBeforeLoop(this);   // re-arm: runBeforeLoop is one-shot
    }
};

static thread_local DriveReactor*                  t_dr       = nullptr;
static thread_local std::unique_ptr<DriveReactor>  t_dr_owner;

void drive_interface_init_reactor(folly::EventBase* eb) {
    t_dr_owner = std::make_unique<DriveReactor>(eb);
    t_dr       = t_dr_owner.get();
}

// ── Low-level uring helpers (reactor thread only) ─────────────────────────────

static folly::coro::Task<int>
do_read(int fd, void* buf, size_t size, uint64_t offset) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueRead(fd, buf, (unsigned int)size, (off_t)offset,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static folly::coro::Task<int>
do_write(int fd, const void* buf, size_t size, uint64_t offset) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueWrite(fd, buf, (unsigned int)size, (off_t)offset,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static folly::coro::Task<int>
do_readv(int fd, const struct iovec* iovs, size_t niov, uint64_t offset) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    // queueReadv copies the iovecs into the IoSqe, so the caller's
    // local iovec array only needs to survive this call, not the await.
    t_dr->uring->queueReadv(fd, {iovs, iovs + niov}, (off_t)offset,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static folly::coro::Task<int>
do_writev(int fd, const struct iovec* iovs, size_t niov, uint64_t offset) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueWritev(fd, {iovs, iovs + niov}, (off_t)offset,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static folly::coro::Task<int>
do_fdatasync(int fd) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueFdatasync(fd,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static folly::coro::Task<int>
do_fallocate(int fd, int mode, uint64_t offset, uint64_t len) {
    folly::Promise<int> p;
    auto sf = p.getSemiFuture();
    t_dr->uring->queueFallocate(fd, mode, (off_t)offset, (off_t)len,
        [p = std::move(p)](int res) mutable { p.setValue(res); });
    co_return co_await std::move(sf).via(t_dr->eb);
}

static std::error_code to_ec(int res) {
    return res < 0 ? std::error_code(-res, std::generic_category())
                   : std::error_code{};
}

// If not on a reactor, hop to one and re-run the call there.
#define ENSURE_REACTOR(hop_expr)                                               \
    if (!t_dr) {                                                               \
        co_return co_await iomgr().spawn_waitable(                             \
            ReactorTarget::any(), (hop_expr));                                 \
    }

// ── Public API — Linux ────────────────────────────────────────────────────────

folly::coro::Task<std::shared_ptr<IoDevice>>
DriveInterface::open_dev(std::string devname, int oflags) {
    int fd = ::open(devname.c_str(), oflags, 0666);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(),
                                "open: " + devname);
    struct stat st{};
    ::fstat(fd, &st);
    co_return std::make_shared<IoDevice>(fd, std::move(devname),
                                         S_ISBLK(st.st_mode));
}

folly::coro::Task<uint64_t>
DriveInterface::get_size(const IoDevice& dev) {
    if (dev.is_block_device) {
        uint64_t sz = 0;
        if (::ioctl(dev.fd, BLKGETSIZE64, &sz) < 0)
            throw std::system_error(errno, std::generic_category(),
                                    "BLKGETSIZE64");
        co_return sz;
    }
    struct stat st{};
    ::fstat(dev.fd, &st);
    co_return static_cast<uint64_t>(st.st_size);
}

folly::coro::Task<std::pair<std::error_code, IOBuffer>>
DriveInterface::read(const IoDevice& dev, IOBuffer buf, uint64_t offset) {
    ENSURE_REACTOR(read(dev, std::move(buf), offset));
    int res = co_await do_read(dev.fd, buf.data(), buf.size(), offset);
    co_return {to_ec(res), std::move(buf)};
}

folly::coro::Task<std::error_code>
DriveInterface::write(const IoDevice& dev, const IOBuffer& buf,
                      uint64_t offset) {
    if (!t_dr) {
        // Off-reactor: capture raw pointer — safe because the caller
        // co_awaits this Task, keeping the IOBuffer alive throughout.
        const void* ptr = buf.data();
        size_t      sz  = buf.size();
        int         fd  = dev.fd;
        co_return to_ec(co_await iomgr().spawn_waitable(
            ReactorTarget::any(), do_write(fd, ptr, sz, offset)));
    }
    co_return to_ec(co_await do_write(dev.fd, buf.data(), buf.size(), offset));
}

folly::coro::Task<std::pair<std::error_code, std::vector<IOBuffer>>>
DriveInterface::readv(const IoDevice& dev, std::vector<IOBuffer> bufs,
                      uint64_t offset) {
    // Hop first so iovecs are only built on the reactor.
    ENSURE_REACTOR(readv(dev, std::move(bufs), offset));

    // queueReadv copies iovecs into the IoSqe. The iov_base pointers
    // reach into bufs.data_; bufs live in this coroutine frame which
    // is alive until the co_await below completes.
    std::vector<struct iovec> iovs;
    iovs.reserve(bufs.size());
    for (auto& b : bufs) iovs.push_back({b.data(), b.size()});

    int res = co_await do_readv(dev.fd, iovs.data(), iovs.size(), offset);
    co_return {to_ec(res), std::move(bufs)};
}

folly::coro::Task<std::error_code>
DriveInterface::writev(const IoDevice& dev, std::vector<IOBuffer> bufs,
                       uint64_t offset) {
    if (!t_dr) {
        // Off-reactor: move bufs into the hopped Task so the kernel's
        // iov_base pointers stay valid until the write CQE arrives.
        int fd = dev.fd;
        co_return to_ec(co_await iomgr().spawn_waitable(
            ReactorTarget::any(),
            [fd, bufs = std::move(bufs), offset]() mutable
                -> folly::coro::Task<int> {
                std::vector<struct iovec> iovs;
                iovs.reserve(bufs.size());
                for (auto& b : bufs) iovs.push_back({b.data(), b.size()});
                co_return co_await do_writev(
                    fd, iovs.data(), iovs.size(), offset);
            }()));
    }

    std::vector<struct iovec> iovs;
    iovs.reserve(bufs.size());
    for (auto& b : bufs) iovs.push_back({b.data(), b.size()});
    co_return to_ec(co_await do_writev(dev.fd, iovs.data(), iovs.size(), offset));
}

folly::coro::Task<std::error_code>
DriveInterface::fsync(const IoDevice& dev) {
    ENSURE_REACTOR(fsync(dev));
    co_return to_ec(co_await do_fdatasync(dev.fd));
}

folly::coro::Task<std::error_code>
DriveInterface::write_zero(const IoDevice& dev, uint64_t size, uint64_t offset) {
    ENSURE_REACTOR(write_zero(dev, size, offset));
    int res;
    if (dev.is_block_device) {
        // BLKZEROOUT is hardware-accelerated on NVMe (typically sub-ms).
        uint64_t range[2] = {offset, size};
        res = ::ioctl(dev.fd, BLKZEROOUT, range);
        if (res < 0) res = -errno;
    } else {
        res = co_await do_fallocate(dev.fd, FALLOC_FL_ZERO_RANGE, offset, size);
    }
    co_return to_ec(res);
}

#undef ENSURE_REACTOR

// ─────────────────────────────────────────────────────────────────────────────
#else // !__linux__
// ── Non-Linux: pread/pwrite on the global CPU thread pool ────────────────────
//
// The EventBase thread is never blocked. Each call hops to folly's CPU
// executor, runs the blocking syscall, then resumes on the original executor.
// ─────────────────────────────────────────────────────────────────────────────

void drive_interface_init_reactor(folly::EventBase*) {}

folly::coro::Task<std::shared_ptr<IoDevice>>
DriveInterface::open_dev(std::string devname, int oflags) {
    int fd = ::open(devname.c_str(), oflags, 0666);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(),
                                "open: " + devname);
    struct stat st{};
    ::fstat(fd, &st);
    co_return std::make_shared<IoDevice>(fd, std::move(devname), false);
}

folly::coro::Task<uint64_t>
DriveInterface::get_size(const IoDevice& dev) {
    struct stat st{};
    ::fstat(dev.fd, &st);
    co_return static_cast<uint64_t>(st.st_size);
}

folly::coro::Task<std::pair<std::error_code, IOBuffer>>
DriveInterface::read(const IoDevice& dev, IOBuffer buf, uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke(
        [fd, buf = std::move(buf), offset]() mutable
            -> folly::coro::Task<std::pair<std::error_code, IOBuffer>> {
            ssize_t n = ::pread(fd, buf.data(), buf.size(), (off_t)offset);
            std::error_code ec = (n < 0)
                ? std::error_code(errno, std::generic_category())
                : std::error_code{};
            co_return {ec, std::move(buf)};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task<std::error_code>
DriveInterface::write(const IoDevice& dev, const IOBuffer& buf,
                      uint64_t offset) {
    int         fd  = dev.fd;
    const void* ptr = buf.data();
    size_t      sz  = buf.size();
    co_return co_await folly::coro::co_invoke(
        [fd, ptr, sz, offset]() -> folly::coro::Task<std::error_code> {
            ssize_t n = ::pwrite(fd, ptr, sz, (off_t)offset);
            co_return (n < 0)
                ? std::error_code(errno, std::generic_category())
                : std::error_code{};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task<std::pair<std::error_code, std::vector<IOBuffer>>>
DriveInterface::readv(const IoDevice& dev, std::vector<IOBuffer> bufs,
                      uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke(
        [fd, bufs = std::move(bufs), offset]() mutable
            -> folly::coro::Task<std::pair<std::error_code,
                                          std::vector<IOBuffer>>> {
            for (auto& b : bufs) {
                ssize_t n = ::pread(fd, b.data(), b.size(), (off_t)offset);
                if (n < 0)
                    co_return {std::error_code(errno, std::generic_category()),
                               std::move(bufs)};
                offset += b.size();
            }
            co_return {std::error_code{}, std::move(bufs)};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task<std::error_code>
DriveInterface::writev(const IoDevice& dev, std::vector<IOBuffer> bufs,
                       uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke(
        [fd, bufs = std::move(bufs), offset]() mutable
            -> folly::coro::Task<std::error_code> {
            for (auto& b : bufs) {
                ssize_t n = ::pwrite(fd, b.data(), b.size(), (off_t)offset);
                if (n < 0)
                    co_return std::error_code(errno, std::generic_category());
                offset += b.size();
            }
            co_return std::error_code{};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task<std::error_code>
DriveInterface::fsync(const IoDevice& dev) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke(
        [fd]() -> folly::coro::Task<std::error_code> {
#ifdef __APPLE__
            // macOS lacks fdatasync; F_FULLFSYNC flushes write-back cache too
            int rc = ::fcntl(fd, F_FULLFSYNC);
#else
            int rc = ::fdatasync(fd);
#endif
            co_return (rc < 0)
                ? std::error_code(errno, std::generic_category())
                : std::error_code{};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

folly::coro::Task<std::error_code>
DriveInterface::write_zero(const IoDevice& dev, uint64_t size, uint64_t offset) {
    int fd = dev.fd;
    co_return co_await folly::coro::co_invoke(
        [fd, size, offset]() -> folly::coro::Task<std::error_code> {
            static constexpr size_t kChunk = 1u << 20;  // 1 MiB
            IOBuffer zbuf{std::min(size, (uint64_t)kChunk)};
            std::memset(zbuf.data(), 0, zbuf.size());
            uint64_t rem = size, off = offset;
            while (rem > 0) {
                size_t n = std::min(rem, (uint64_t)zbuf.size());
                if (::pwrite(fd, zbuf.data(), n, (off_t)off) < 0)
                    co_return std::error_code(errno, std::generic_category());
                off += n;
                rem -= n;
            }
            co_return std::error_code{};
        }).scheduleOn(folly::getGlobalCPUExecutor().get());
}

#endif // __linux__

} // namespace homestore
