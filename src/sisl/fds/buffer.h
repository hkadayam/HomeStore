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

struct Blob {
protected:
    uint8_t* bytes_{nullptr};
    uint32_t size_{0};
#ifdef _DEBUG
    bool is_const_{false};
#endif

public:
    Blob() = default;
    Blob(uint8_t* b, uint32_t s) : bytes_{b}, size_{s} {}
    Blob(uint8_t const* b, uint32_t s) : bytes_{const_cast< uint8_t* >(b)}, size_{s} {
#ifdef _DEBUG
        is_const_ = true;
#endif
    }

    uint8_t* bytes() {
        DEBUG_ASSERT_EQ(is_const_, false, "Trying to access writeable bytes with const declaration");
        return bytes_;
    }
    uint32_t size() const { return size_; }
    uint8_t const* cbytes() const { return bytes_; }

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
    void set_size(uint32_t s) { size_ = s; }
};

// ── SgList / SgIterator ───────────────────────────────────────────────────────

using SgIovs = folly::small_vector< iovec, 4 >;

struct SgList {
    uint64_t size{0}; // total size of data pointed by iovs
    SgIovs iovs;
};

struct SgIterator {
    SgIterator(const SgIovs& v) : input_iovs_{v} { assert(v.size() > 0); }

    SgIovs next_iovs(uint32_t size) {
        SgIovs ret_iovs;
        auto remain_size = size;
        while ((remain_size > 0) && (cur_index_ < input_iovs_.size())) {
            const auto& inp_iov = input_iovs_[cur_index_];
            iovec this_iov;
            this_iov.iov_base = static_cast< uint8_t* >(inp_iov.iov_base) + cur_offset_;
            if (remain_size < inp_iov.iov_len - cur_offset_) {
                this_iov.iov_len = remain_size;
                cur_offset_ += remain_size;
            } else {
                this_iov.iov_len = inp_iov.iov_len - cur_offset_;
                ++cur_index_;
                cur_offset_ = 0;
            }
            ret_iovs.push_back(this_iov);
            assert(remain_size >= this_iov.iov_len);
            remain_size -= this_iov.iov_len;
        }
        return ret_iovs;
    }

    void move_offset(uint32_t size) {
        auto remain_size = size;
        const auto n = input_iovs_.size();
        for (; (remain_size > 0) && (cur_index_ < n); ++cur_index_, cur_offset_ = 0) {
            const auto& inp_iov = input_iovs_[cur_index_];
            if (remain_size < inp_iov.iov_len - cur_offset_) {
                cur_offset_ += remain_size;
                return;
            }
            remain_size -= inp_iov.iov_len - cur_offset_;
        }
    }

    const SgIovs& input_iovs_;
    uint64_t cur_offset_{0};
    size_t cur_index_{0};
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

// ── IoBlob ────────────────────────────────────────────────────────────────────

struct IoBlob;
using IoBlobList = folly::small_vector< sisl::IoBlob, 4 >;

struct IoBlob : public Blob {
protected:
    bool aligned_{false};

public:
    IoBlob() = default;
    IoBlob(size_t sz, uint32_t align_size = 512, Buftag tag = Buftag::common) {
#ifdef _DEBUG
        buf_alloc_and_init(sz, align_size, tag, 0xEE);
#else
        buf_alloc(sz, align_size, tag);
#endif
    }
    IoBlob(uint8_t* bytes, uint32_t size, bool is_aligned) : Blob(bytes, size), aligned_{is_aligned} {}
    IoBlob(uint8_t const* bytes, uint32_t size, bool is_aligned) : Blob(bytes, size), aligned_{is_aligned} {}
    ~IoBlob() = default;

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

    bool is_aligned() const { return aligned_; }

    static IoBlob from_string(const std::string& s) {
        return IoBlob{r_cast< const uint8_t* >(s.data()), uint32_cast(s.size()), false};
    }

    static IoBlobList sg_list_to_ioblob_list(const SgList& sglist) {
        IoBlobList ret_list;
        for (const auto& iov : sglist.iovs) {
            ret_list.emplace_back(r_cast< uint8_t* >(const_cast< void* >(iov.iov_base)), uint32_cast(iov.iov_len),
                                  false);
        }
        return ret_list;
    }
};

// ── IoBlobSafe ────────────────────────────────────────────────────────────────
// Owns its buffer: allocates on construction, frees on destruction.

struct IoBlobSafe final : public IoBlob {
public:
    Buftag tag_{Buftag::common};

public:
    IoBlobSafe() = default;
    IoBlobSafe(uint32_t sz, uint32_t alignment = 512, Buftag tag = Buftag::common) :
            IoBlob(sz, alignment, tag), tag_{tag} {}
    IoBlobSafe(uint8_t* bytes, uint32_t size, bool is_aligned) : IoBlob(bytes, size, is_aligned) {}
    IoBlobSafe(uint8_t const* bytes, uint32_t size, bool is_aligned) : IoBlob(bytes, size, is_aligned) {}
    ~IoBlobSafe() {
        if (Blob::bytes_ != nullptr) {
            IoBlob::buf_free(tag_);
        }
    }

    IoBlobSafe(IoBlobSafe const&) = delete;
    IoBlobSafe(IoBlobSafe&& other) : IoBlob(std::move(other)), tag_(other.tag_) {
        other.bytes_ = nullptr;
        other.size_ = 0;
    }

    IoBlobSafe& operator=(IoBlobSafe const&) = delete;
    IoBlobSafe& operator=(IoBlobSafe&& other) {
        if (Blob::bytes_ != nullptr) {
            this->buf_free(tag_);
        }
        *static_cast< IoBlob* >(this) = std::move(*static_cast< IoBlob* >(&other));
        tag_ = other.tag_;
        other.bytes_ = nullptr;
        other.size_ = 0;
        return *this;
    }

    void buf_alloc(size_t sz, uint32_t align_size = 512) { IoBlob::buf_alloc(sz, align_size, tag_); }
};

// ── ByteArray / ByteView ──────────────────────────────────────────────────────

using ByteArrayImpl = IoBlobSafe;
using ByteArray = shared< IoBlobSafe >;

inline ByteArray make_byte_array(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) {
    return std::make_shared< IoBlobSafe >(sz, alignment, tag);
}

inline ByteArray make_byte_array(IoBlobSafe&& blob) { return std::make_shared< IoBlobSafe >(std::move(blob)); }

struct ByteView {
public:
    ByteView() = default;
    ByteView(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) {
        base_buf_ = make_byte_array(sz, alignment, tag);
        view_.set_bytes(base_buf_->cbytes());
        view_.set_size(base_buf_->size());
    }
    ByteView(ByteArray buf) : ByteView(std::move(buf), 0u, buf->size()) {}
    ByteView(ByteArray buf, uint32_t offset, uint32_t sz) {
        base_buf_ = std::move(buf);
        view_.set_bytes(base_buf_->cbytes() + offset);
        view_.set_size(sz);
    }
    ByteView(const ByteView& v, uint32_t offset, uint32_t sz) {
        DEBUG_ASSERT_GE(v.view_.size(), sz + offset);
        base_buf_ = v.base_buf_;
        view_.set_bytes(v.view_.cbytes() + offset);
        view_.set_size(sz);
    }
    ByteView(const sisl::IoBlob& b) : ByteView(b.size(), b.is_aligned()) {}

    ~ByteView() = default;
    ByteView(const ByteView&) = default;
    ByteView& operator=(const ByteView&) = default;

    ByteView(ByteView&& other) {
        base_buf_ = std::move(other.base_buf_);
        view_ = std::move(other.view_);
    }
    ByteView& operator=(ByteView&& other) {
        base_buf_ = std::move(other.base_buf_);
        view_ = std::move(other.view_);
        return *this;
    }

    Blob get_blob() const { return view_; }
    uint8_t const* bytes() const { return view_.cbytes(); }
    uint32_t size() const { return view_.size(); }

    void move_forward(uint32_t by) {
        DEBUG_ASSERT_GE(view_.size(), by, "Size greater than move forward request by");
        view_.set_bytes(view_.cbytes() + by);
        view_.set_size(view_.size() - by);
        validate();
    }

    ByteArray extract(uint32_t alignment = 0) const {
        if (can_do_shallow_copy()) {
            return base_buf_;
        }
        auto base_buf = make_byte_array(view_.size(), alignment, base_buf_->tag_);
        std::memcpy(base_buf->bytes(), view_.cbytes(), view_.size());
        return base_buf;
    }

    bool can_do_shallow_copy() const {
        return (view_.cbytes() == base_buf_->cbytes()) && (view_.size() == base_buf_->size());
    }
    void set_size(uint32_t sz) { view_.set_size(sz); }
    void validate() const {
        DEBUG_ASSERT_LE((void*)(base_buf_->cbytes() + base_buf_->size()), (void*)(view_.cbytes() + view_.size()),
                        "Invalid ByteView");
    }
    std::string get_string() const { return std::string(r_cast< const char* >(bytes()), uint64_cast(size())); }

private:
    ByteArray base_buf_;
    Blob view_;
};

// ── BufBuilder ────────────────────────────────────────────────────────────────

struct BufBuilder {
public:
    BufBuilder(uint32_t sz, uint32_t alignment = 0, Buftag tag = Buftag::common) : alignment_{alignment} {
        buf_ = make_byte_array(sz, alignment, tag);
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

    ByteView view() const { return ByteView{buf_, 0, occupied_space()}; }
    uint8_t* bytes() const { return buf_->bytes(); }
    uint32_t occupied_space() const { return cur_ptr_ - buf_->bytes(); }
    uint32_t available_space() const { return buf_->size() - occupied_space(); }

private:
    ByteArray buf_;
    uint32_t alignment_{0};
    uint8_t* cur_ptr_{nullptr};
};

} // namespace sisl