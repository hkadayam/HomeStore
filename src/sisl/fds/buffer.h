/*********************************************************************************
 *
 * Author/Developer(s): Harihara Kadayam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <vector>

#include <sys/uio.h>
#ifdef __linux__
#include <malloc.h>
#endif

#include <folly/small_vector.h>

#include "common/defs.h"
#include "sisl/metrics/metrics.h"
#include "sisl/fds/utils.h"

#ifndef NDEBUG
#ifndef _DEBUG
#define _DEBUG
#endif
#endif

namespace sisl {

// ── Blob ──────────────────────────────────────────────────────────────────────
// Non-owning pointer + size.  Debug builds track constness via is_const_;
// release builds are zero-overhead.  uint32_t size is intentional (4 GB cap).

// ── IoBuf (abstract base) ──────────────────────────────────────────────────
// Common abstract base implemented by every concrete IO-buffer type — IoBufSpan (non-owning view),
// IoBufOwn (owning aligned), IoBufView (non-owning slice with shared lifetime).  Lower IO layers
// (DriveInterface / PhysicalDev / VirtualDev / RawBlkStream) accept `IoBuf const&` / `IoBuf&`
// and rely on the virtual accessors to extract bytes, length, and alignment for direct IO.
class IoBuf {
public:
    virtual ~IoBuf() = default;
    virtual uint8_t* bytes() = 0;
    virtual uint8_t const* cbytes() const = 0;
    virtual uint32_t size() const = 0;
    virtual bool is_aligned() const = 0;
    /// True if this IoBuf manages its own memory lifetime (RAII owning, refcounted, or held-alive via shared
    /// pointer).  False for pure non-owning views where the caller must guarantee the underlying memory
    /// outlives all uses.  Used by the vectored I/O path to assert that scatter-gather sources are safe to
    /// pass across async boundaries.
    virtual bool is_safe() const = 0;
};

struct Blob {
protected:
    uint8_t* bytes_{nullptr};
    uint32_t size_{0};
#ifdef _DEBUG
    bool is_const_{false};
#endif

public:
    Blob() = default;
    Blob(uint8_t* b, uint32_t s) : bytes_{b}, size_{s} {
    }
    Blob(uint8_t const* b, uint32_t s) : bytes_{const_cast< uint8_t* >(b)}, size_{s} {
#ifdef _DEBUG
        is_const_ = true;
#endif
    }

    uint8_t* bytes() {
        DEBUG_ASSERT_EQ(is_const_, false, "Trying to access writeable bytes with const declaration");
        return bytes_;
    }
    uint32_t size() const {
        return size_;
    }
    uint8_t const* cbytes() const {
        return bytes_;
    }

    void set_bytes(uint8_t* b) {
        DEBUG_ASSERT_EQ(is_const_, false, "Trying to access writeable bytes with const declaration");
        bytes_ = b;
    }
    void set_bytes(uint8_t const* b) {
#ifdef _DEBUG
        is_const_ = false;
#endif
        bytes_ = const_cast< uint8_t* >(b);
    }
    void set_size(uint32_t s) {
        size_ = s;
    }
};

// ── SgList ────────────────────────────────────────────────────────────────────
// Scatter-gather list of polymorphic IoBuf pointers.  Used by the device I/O writev/readv path so each
// element retains its concrete type, alignment, and lifetime semantics through the call.  Each IoBuf*
// must outlive the await of the operation; SgList itself does not own the buffers.
struct SgList {
    folly::small_vector< IoBuf*, 4 > bufs;

    /// Sum of `b->size()` across all entries — total bytes the SG list represents.
    uint32_t total_size() const {
        uint32_t s = 0;
        for (auto const* b : bufs) {
            s += b->size();
        }
        return s;
    }
};

// ── Buftag ────────────────────────────────────────────────────────────────────
// Used by both the sisl allocator (for per-tag memory tracking) and iomgr
// (for pool-vs-regular allocation decisions).

enum class Buftag : uint8_t {
    common = 0,
    bitset = 1,
    superblk = 2,
    metablk = 3,
    logread = 4,
    logwrite = 5,
    compression = 6,
    data_journal = 7,
    btree_journal = 8,
    btree_node = 9,
    Sentinel = 10,
};
static constexpr size_t kNumBuftags{static_cast< size_t >(Buftag::Sentinel)};

// ── AlignedAllocatorMetrics ───────────────────────────────────────────────────
// Tracks allocated bytes per Buftag.  Published as gauges so dashboards/alerts
// can catch per-subsystem memory leaks.

class AlignedAllocatorMetrics : public MetricsGroup {
public:
    AlignedAllocatorMetrics(const AlignedAllocatorMetrics&) = delete;
    AlignedAllocatorMetrics(AlignedAllocatorMetrics&&) noexcept = delete;
    AlignedAllocatorMetrics& operator=(const AlignedAllocatorMetrics&) = delete;
    AlignedAllocatorMetrics& operator=(AlignedAllocatorMetrics&&) noexcept = delete;

    AlignedAllocatorMetrics() : MetricsGroup("AlignedAllocation", "Singleton") {
        static constexpr std::array< std::string_view, kNumBuftags > kTagNames{
            "common",   "bitset",      "superblk",     "metablk",       "logread",
            "logwrite", "compression", "data_journal", "btree_journal", "btree_node"};
        for (size_t t = 0; t < kNumBuftags; ++t) {
            const std::string name = "buftag_" + std::string{kTagNames[t]};
            tag_idx_[t] = impl_ptr_->register_counter(name, name, PublishAs::Gauge);
        }
        register_me_to_farm();
    }

    void increment(Buftag tag, size_t sz) { impl_ptr_->counter_increment(tag_idx_[static_cast< size_t >(tag)], sz); }
    void decrement(Buftag tag, size_t sz) { impl_ptr_->counter_decrement(tag_idx_[static_cast< size_t >(tag)], sz); }

private:
    std::array< size_t, kNumBuftags > tag_idx_{};
};

// Lazily-created singleton metrics instance — thread-safe per C++11.
inline AlignedAllocatorMetrics& aligned_alloc_metrics() {
    static AlignedAllocatorMetrics s_instance;
    return s_instance;
}

// ── Aligned allocation free functions ─────────────────────────────────────────
// Direct posix_memalign wrappers with per-tag metrics.
// No virtual dispatch, no singleton allocator, no separate Impl class.

inline size_t buf_usable_size(void* p) {
#ifdef __linux__
    return ::malloc_usable_size(p);
#else
    (void)p;
    return 0;
#endif
}

inline uint8_t* aligned_alloc(size_t align, size_t sz, Buftag tag = Buftag::common) {
    void* ptr{nullptr};
    if (::posix_memalign(&ptr, align, round_up(sz, align)) != 0) {
        throw std::bad_alloc{};
    }
    aligned_alloc_metrics().increment(tag, buf_usable_size(ptr));
    return static_cast< uint8_t* >(ptr);
}

inline void aligned_free(uint8_t* p, Buftag tag = Buftag::common) {
    if (!p)
        return;
    aligned_alloc_metrics().decrement(tag, buf_usable_size(static_cast< void* >(p)));
    ::free(p);
}

inline uint8_t* aligned_realloc(uint8_t* old_buf, size_t align, size_t new_sz, size_t old_sz = 0,
                                Buftag tag = Buftag::common) {
    const size_t old_real{(old_sz != 0) ? old_sz : buf_usable_size(static_cast< void* >(old_buf))};
    if (old_real >= new_sz)
        return old_buf;
    uint8_t* const new_buf{aligned_alloc(align, new_sz, tag)};
    if (old_buf) {
        std::memcpy(new_buf, old_buf, old_real);
        aligned_free(old_buf, tag);
    }
    return new_buf;
}

// ── AlignedDeleter / AlignedUniquePtr / AlignedSharedPtr ─────────────────────

template < typename T, Buftag Tag = Buftag::common >
struct AlignedDeleter {
    void operator()(T* p) {
        if constexpr (std::is_destructible_v< std::decay_t< T > >) {
            p->~T();
        }
        aligned_free(r_cast< uint8_t* >(p), Tag);
    }
};

template < typename T, Buftag Tag = Buftag::common >
class AlignedUniquePtr : public std::unique_ptr< T, AlignedDeleter< T, Tag > > {
public:
    template < class... Args >
    static AlignedUniquePtr< T, Tag > make(size_t align, Args&&... args) {
        return make_sized(align, sizeof(T), std::forward< Args >(args)...);
    }

    template < class... Args >
    static AlignedUniquePtr< T, Tag > make_sized(size_t align, size_t size, Args&&... args) {
        return AlignedUniquePtr< T, Tag >(new (aligned_alloc(align, size, Tag)) T(std::forward< Args >(args)...));
    }

    AlignedUniquePtr() = default;
    AlignedUniquePtr(T* p) : std::unique_ptr< T, AlignedDeleter< T, Tag > >(p) {}
};

template < typename T, Buftag Tag = Buftag::common >
class AlignedSharedPtr : public std::shared_ptr< T > {
public:
    template < class... Args >
    static std::shared_ptr< T > make(size_t align, Args&&... args) {
        return make_sized(align, sizeof(T), std::forward< Args >(args)...);
    }

    template < class... Args >
    static std::shared_ptr< T > make_sized(size_t align, size_t size, Args&&... args) {
        return std::shared_ptr< T >(new (aligned_alloc(align, size, Tag)) T(std::forward< Args >(args)...),
                                    AlignedDeleter< T, Tag >());
    }

    AlignedSharedPtr(T* p) : std::shared_ptr< T >(p) {}
};

// ── AlignedTypeAllocator / AlignedVector ─────────────────────────────────────

template < typename T, std::size_t Alignment = 512 >
class AlignedTypeAllocator {
    static_assert(Alignment >= alignof(T),
                  "Alignment is smaller than T's natural alignment; access will result in crashes.");

public:
    template < class U >
    struct rebind {
        using other = AlignedTypeAllocator< U, Alignment >;
    };

    constexpr AlignedTypeAllocator() noexcept = default;
    constexpr AlignedTypeAllocator(const AlignedTypeAllocator&) noexcept = default;

    template < typename U >
    constexpr AlignedTypeAllocator(AlignedTypeAllocator< U, Alignment > const&) noexcept {}

    T* allocate(std::size_t nelems) {
        if (nelems > std::numeric_limits< std::size_t >::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return r_cast< T* >(aligned_alloc(Alignment, nelems * sizeof(T), Buftag::common));
    }

    void deallocate(T* ptr, [[maybe_unused]] std::size_t) { aligned_free(uintptr_cast(ptr), Buftag::common); }
};

template < typename T, std::size_t Alignment = 512 >
using AlignedVector = std::vector< T, AlignedTypeAllocator< T, Alignment > >;

// ── IoBufSpan ────────────────────────────────────────────────────────────────────

struct IoBufSpan;
using IoBufSpanList = folly::small_vector< sisl::IoBufSpan, 4 >;

struct IoBufSpan : public Blob, public IoBuf {
protected:
    bool aligned_{false};

public:
    IoBufSpan() = default;
    IoBufSpan(size_t sz, uint32_t align_size = 512, Buftag tag = Buftag::common) {
#ifdef _DEBUG
        buf_alloc_and_init(sz, align_size, tag, 0xEE);
#else
        buf_alloc(sz, align_size, tag);
#endif
    }
    IoBufSpan(uint8_t* bytes, uint32_t size, bool is_aligned) : Blob(bytes, size), aligned_{is_aligned} {
    }
    IoBufSpan(uint8_t const* bytes, uint32_t size, bool is_aligned) : Blob(bytes, size), aligned_{is_aligned} {
    }
    ~IoBufSpan() override = default;

    // ── IoBuf overrides ────────────────────────────────────────────────────
    uint8_t* bytes() override { return Blob::bytes(); }
    uint8_t const* cbytes() const override { return Blob::cbytes(); }
    uint32_t size() const override { return Blob::size(); }
    bool is_aligned() const override { return aligned_; }
    /// Non-owning view — caller must guarantee underlying memory outlives every use.  IoBufOwn overrides
    /// to true since it RAII-manages its own buffer.
    bool is_safe() const override { return false; }

    void buf_alloc(size_t sz, uint32_t align_size = 512, Buftag tag = Buftag::common) {
        aligned_ = (align_size != 0);
        Blob::size_ = sz;
        Blob::bytes_ = aligned_ ? aligned_alloc(align_size, sz, tag) : static_cast< uint8_t* >(::malloc(sz));
    }

    void buf_alloc_and_init(size_t sz, uint32_t align_size = 512, Buftag tag = Buftag::common, uint8_t init_val = 0) {
        buf_alloc(sz, align_size, tag);
        std::memset(Blob::bytes_, init_val, sz);
    }

    void buf_free(Buftag tag = Buftag::common) const {
        aligned_ ? aligned_free(Blob::bytes_, tag) : ::free(Blob::bytes_);
    }

    void buf_realloc(size_t new_size, uint32_t align_size = 512, [[maybe_unused]] Buftag tag = Buftag::common) {
        uint8_t* new_buf{nullptr};
        if (aligned_) {
            new_buf = aligned_realloc(Blob::bytes_, align_size, new_size, Blob::size_, tag);
        } else if (align_size != 0) {
            new_buf = aligned_alloc(align_size, new_size, tag);
            std::memcpy(new_buf, Blob::bytes_, std::min(new_size, static_cast< size_t >(Blob::size_)));
            ::free(Blob::bytes_);
        } else {
            new_buf = static_cast< uint8_t* >(::realloc(Blob::bytes_, new_size));
        }
        Blob::size_ = new_size;
        Blob::bytes_ = new_buf;
    }

    static IoBufSpan from_string(const std::string& s) {
        return IoBufSpan{r_cast< const uint8_t* >(s.data()), uint32_cast(s.size()), false};
    }

};

// ── IoBufOwn ────────────────────────────────────────────────────────────────
// Owns its buffer: allocates on construction, frees on destruction.

struct IoBufOwn final : public IoBufSpan {
public:
    Buftag tag_{Buftag::common};

public:
    IoBufOwn() = default;
    IoBufOwn(uint32_t sz, uint32_t alignment = 512, Buftag tag = Buftag::common) :
            IoBufSpan(sz, alignment, tag), tag_{tag} {}
    IoBufOwn(uint8_t* bytes, uint32_t size, bool is_aligned) : IoBufSpan(bytes, size, is_aligned) {}
    IoBufOwn(uint8_t const* bytes, uint32_t size, bool is_aligned) : IoBufSpan(bytes, size, is_aligned) {}
    ~IoBufOwn() {
        if (Blob::bytes_ != nullptr) {
            IoBufSpan::buf_free(tag_);
        }
    }

    /// Owns its buffer via RAII alloc/free.
    bool is_safe() const override { return true; }

    IoBufOwn(IoBufOwn const&) = delete;
    IoBufOwn(IoBufOwn&& other) : IoBufSpan(std::move(other)), tag_(other.tag_) {
        other.bytes_ = nullptr;
        other.size_ = 0;
    }

    IoBufOwn& operator=(IoBufOwn const&) = delete;
    IoBufOwn& operator=(IoBufOwn&& other) {
        if (Blob::bytes_ != nullptr) {
            this->buf_free(tag_);
        }
        *static_cast< IoBufSpan* >(this) = std::move(*static_cast< IoBufSpan* >(&other));
        tag_ = other.tag_;
        other.bytes_ = nullptr;
        other.size_ = 0;
        return *this;
    }

    void buf_alloc(size_t sz, uint32_t align_size = 512) { IoBufSpan::buf_alloc(sz, align_size, tag_); }

    // Release ownership of the internal buffer into a shared_ptr<uint8_t>.  After this call the IoBufOwn is empty
    // and will NOT free the buffer on destruction.  The returned shared_ptr uses the same free path as buf_free().
    shared< uint8_t > release_to_shared_ptr() {
        auto* p = bytes_;
        auto t = tag_;
        auto a = aligned_;
        bytes_ = nullptr;
        size_ = 0;
        return shared< uint8_t >(p, [t, a](uint8_t* ptr) { a ? aligned_free(ptr, t) : ::free(ptr); });
    }
};

// ── IoBufShared / IoBufView ──────────────────────────────────────────────────────

using IoBufSharedImpl = IoBufOwn;
using IoBufShared = shared< IoBufOwn >;

inline IoBufShared make_io_buf_shared(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) {
    return std::make_shared< IoBufOwn >(sz, alignment, tag);
}

inline IoBufShared make_io_buf_shared(IoBufOwn&& blob) {
    return std::make_shared< IoBufOwn >(std::move(blob));
}

/// Zero-copy IoBufShared from a shared_ptr<uint8_t>.  The IoBufOwn wrapper is heap-allocated (~24 bytes) but the
/// actual buffer is not copied.  The shared_ptr<uint8_t> is captured in the deleter and freed when the IoBufShared
/// refcount reaches 0.
inline IoBufShared make_io_buf_shared(std::shared_ptr< uint8_t > owner, uint32_t size, bool is_aligned = true) {
    auto* raw = owner.get();
    return IoBufShared(new IoBufOwn(raw, size, is_aligned), [o = std::move(owner)](IoBufOwn* p) {
        // Null out bytes_ so IoBufOwn's destructor skips buf_free — the captured `o` shared_ptr owns the buffer.
        // static_cast is required to disambiguate between the uint8_t* and uint8_t const* overloads of set_bytes.
        p->set_bytes(static_cast< uint8_t* >(nullptr));
        delete p;
    });
}

class IoBufView : public IoBuf {
public:
    IoBufView() = default;
    IoBufView(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) {
        base_buf_ = make_io_buf_shared(sz, alignment, tag);
        view_.set_bytes(base_buf_->cbytes());
        view_.set_size(base_buf_->size());
    }
    IoBufView(IoBufShared buf) {
        base_buf_ = std::move(buf);
        view_.set_bytes(base_buf_->cbytes());
        view_.set_size(base_buf_->size());
    }
    IoBufView(IoBufShared buf, uint32_t offset, uint32_t sz) {
        base_buf_ = std::move(buf);
        view_.set_bytes(base_buf_->cbytes() + offset);
        view_.set_size(sz);
    }
    IoBufView(const IoBufView& v, uint32_t offset, uint32_t sz) {
        DEBUG_ASSERT_GE(v.view_.size(), sz + offset);
        base_buf_ = v.base_buf_;
        view_.set_bytes(v.view_.cbytes() + offset);
        view_.set_size(sz);
    }
    IoBufView(const sisl::IoBufSpan& b) : IoBufView(b.size(), b.is_aligned()) {}

    ~IoBufView() override = default;
    IoBufView(const IoBufView&) = default;
    IoBufView& operator=(const IoBufView&) = default;

    IoBufView(IoBufView&& other) {
        base_buf_ = std::move(other.base_buf_);
        view_ = std::move(other.view_);
    }
    IoBufView& operator=(IoBufView&& other) {
        base_buf_ = std::move(other.base_buf_);
        view_ = std::move(other.view_);
        return *this;
    }

    Blob get_blob() const { return view_; }

    // ── IoBuf overrides ────────────────────────────────────────────────────
    uint8_t* bytes() override { return view_.bytes(); }
    uint8_t const* cbytes() const override { return view_.cbytes(); }
    uint32_t size() const override { return view_.size(); }
    bool is_aligned() const override { return base_buf_ ? base_buf_->is_aligned() : false; }
    /// Underlying memory is kept alive via base_buf_ (shared_ptr to IoBufOwn).
    bool is_safe() const override { return true; }

    void move_forward(uint32_t by) {
        DEBUG_ASSERT_GE(view_.size(), by, "Size greater than move forward request by");
        view_.set_bytes(view_.cbytes() + by);
        view_.set_size(view_.size() - by);
        validate();
    }

    IoBufShared extract(uint32_t alignment = 0) const {
        if (can_do_shallow_copy()) {
            return base_buf_;
        }
        auto base_buf = make_io_buf_shared(view_.size(), alignment, base_buf_->tag_);
        std::memcpy(base_buf->bytes(), view_.cbytes(), view_.size());
        return base_buf;
    }

    bool can_do_shallow_copy() const {
        return (view_.cbytes() == base_buf_->cbytes()) && (view_.size() == base_buf_->size());
    }
    void set_size(uint32_t sz) { view_.set_size(sz); }
    void validate() const {
        DEBUG_ASSERT_LE((void*)(base_buf_->cbytes() + base_buf_->size()), (void*)(view_.cbytes() + view_.size()),
                        "Invalid IoBufView");
    }
    std::string get_string() const { return std::string(r_cast< const char* >(cbytes()), uint64_cast(size())); }

private:
    IoBufShared base_buf_;
    Blob view_;
};

// ── BufBuilder ────────────────────────────────────────────────────────────────

struct BufBuilder {
public:
    BufBuilder(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) : alignment_{alignment} {
        buf_ = make_io_buf_shared(sz, alignment, tag);
        cur_ptr_ = buf_->bytes();
    }

    void append(Blob const& incoming_buf) {
        if (available_space() < incoming_buf.size()) {
            auto const increase_size = std::max(incoming_buf.size(), uint32_cast(buf_->size() * 1.5));
            auto const cur_offset = occupied_space();
            buf_->buf_realloc(buf_->size() + increase_size, alignment_);
            cur_ptr_ = buf_->bytes() + cur_offset;
        }
        std::memcpy(cur_ptr_, incoming_buf.cbytes(), incoming_buf.size());
        cur_ptr_ += incoming_buf.size();
    }

    IoBufView view() const { return IoBufView{buf_, 0, occupied_space()}; }
    uint8_t* bytes() const { return buf_->bytes(); }
    uint32_t occupied_space() const { return cur_ptr_ - buf_->bytes(); }
    uint32_t available_space() const { return buf_->size() - occupied_space(); }

private:
    IoBufShared buf_;
    uint32_t alignment_{0};
    uint8_t* cur_ptr_{nullptr};
};

// ── LargeBufBuilder ──────────────────────────────────────────────────────────
// Chain of fixed-size aligned IoBufOwn buffers.  append() memcpys into the current buffer; when full, a new one is
// allocated — no realloc/copy of prior data.  for_each_piece() walks the chain calling cb(IoBufOwn&) with the
// buffer's size already set to the used byte count, ready to pass directly to I/O layers.

struct LargeBufBuilder {
public:
    LargeBufBuilder() = default;
    explicit LargeBufBuilder(uint32_t buf_capacity, uint32_t alignment = 512, Buftag tag = Buftag::common) :
            buf_capacity_{buf_capacity}, alignment_{alignment}, tag_{tag} {
        bufs_.reserve(16);
    }

    void append(Blob const& data) {
        auto const* src = data.cbytes();
        uint32_t remaining = data.size();

        while (remaining > 0) {
            if (bufs_.empty() || cur_offset_ >= buf_capacity_) {
                if (!bufs_.empty())
                    bufs_.back().set_size(cur_offset_); // close prior partially-filled buf
                bufs_.emplace_back(buf_capacity_, alignment_, tag_);
                cur_offset_ = 0;
            }
            uint32_t const space = buf_capacity_ - cur_offset_;
            uint32_t const copy_len = std::min(remaining, space);
            std::memcpy(bufs_.back().bytes() + cur_offset_, src, copy_len);
            cur_offset_ += copy_len;
            src += copy_len;
            remaining -= copy_len;
        }
    }

    // Reserve `size` contiguous bytes in a buffer and invoke fill(sisl::Blob) to populate them.  Returns without the
    // caller having to memcpy from any source.  If the current buf can't fit, starts a new one sized to
    // max(default_capacity, size) — so oversized emplaces get their own dedicated buf.
    template < typename FillFn >
    void emplace(uint32_t size, FillFn&& fill) {
        if (bufs_.empty() || size > (buf_capacity_ > cur_offset_ ? buf_capacity_ - cur_offset_ : 0)) {
            if (!bufs_.empty())
                bufs_.back().set_size(cur_offset_); // close prior partially-filled buf
            bufs_.emplace_back(std::max(buf_capacity_, size), alignment_, tag_);
            cur_offset_ = 0;
        }
        uint8_t* ptr = bufs_.back().bytes() + cur_offset_;
        std::forward< FillFn >(fill)(Blob{ptr, size});
        cur_offset_ += size;
    }

    // Move all buffers out as a vector; the last (partially filled) buffer's size is set to the used byte count.  All
    // earlier buffers already carry size == buf_capacity_ from construction.  Builder is empty after this call.
    std::vector< IoBufOwn > move_all_bufs() {
        if (!bufs_.empty()) {
            bufs_.back().set_size(cur_offset_);
        }
        cur_offset_ = 0;
        return std::move(bufs_);
    }

    // Consume the builder: moves each buffer out to cb with its size set to the used byte count.
    // The builder is empty after this call.  cb signature: void(IoBufOwn&&).
    template < typename Cb >
    void consume(Cb&& cb) {
        for (size_t i = 0; i < bufs_.size(); ++i) {
            uint32_t const used = (i + 1 < bufs_.size()) ? buf_capacity_ : cur_offset_;
            if (used > 0) {
                bufs_[i].set_size(used);
                cb(std::move(bufs_[i]));
            }
        }
        bufs_.clear();
        cur_offset_ = 0;
    }

    uint64_t total_bytes() const {
        if (bufs_.empty()) {
            return 0;
        }
        return (bufs_.size() - 1) * uint64_cast(buf_capacity_) + cur_offset_;
    }

    bool empty() const { return bufs_.empty() || (bufs_.size() == 1 && cur_offset_ == 0); }

    void clear() {
        bufs_.clear();
        cur_offset_ = 0;
    }

private:
    std::vector< IoBufOwn > bufs_;
    uint32_t cur_offset_{0};
    uint32_t buf_capacity_{256 * 4096}; // default 1 MB
    uint32_t alignment_{512};
    Buftag tag_{Buftag::common};
};

} // namespace sisl
