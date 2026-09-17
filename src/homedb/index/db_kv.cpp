#include "homedb/index/db_kv.h"

#include <cstring>

#include <fmt/format.h>

namespace homedb {

static sisl::IoBufShared alloc_owned(sisl::Blob const& src) {
    auto buf = sisl::make_io_buf_shared(src.size(), /*alignment=*/0, sisl::Buftag::common);
    if (src.size() > 0) {
        std::memcpy(buf->bytes(), src.cbytes(), src.size());
    }
    return buf;
}

// ────────────────────────────────────────────── DbKey ────────────────────────────────────────────────────────────

DbKey::DbKey(sisl::Blob const& src, std::optional< size_t > fixed_size) : view_{src}, fixed_size_{fixed_size} {}

int DbKey::compare(homestore::BtreeKey const& other) const {
    auto const& rhs = s_cast< DbKey const& >(other);
    auto const l_sz = size();
    auto const r_sz = rhs.size();
    auto const min_sz = std::min(l_sz, r_sz);
    if (min_sz > 0) {
        int const c = std::memcmp(data(), rhs.data(), min_sz);
        if (c != 0) {
            return c;
        }
    }
    if (l_sz < r_sz) return -1;
    if (l_sz > r_sz) return 1;
    return 0;
}

sisl::Blob DbKey::serialize() const { return view_; }

uint32_t DbKey::serialized_size() const { return fixed_size_ ? to_u32(*fixed_size_) : size(); }

void DbKey::deserialize(sisl::Blob const& b, bool copy) {
    if (copy) {
        owner_ = alloc_owned(b);
        view_ = sisl::Blob{owner_->cbytes(), owner_->size()};
    } else {
        owner_.reset();
        view_ = b;
    }
}

std::string DbKey::to_string() const {
    auto const sz = size();
    auto const* p = data();
    std::string out;
    out.reserve(2 * std::min(size_t{sz}, size_t{8}) + 6);
    out.push_back('[');
    size_t const show = std::min(size_t{sz}, size_t{8});
    for (size_t i = 0; i < show; ++i) {
        out += fmt::format("{:02x}", p[i]);
    }
    if (sz > show) {
        out += fmt::format("..({})", sz);
    }
    out.push_back(']');
    return out;
}

// ────────────────────────────────────────────── DbValue ──────────────────────────────────────────────────────────

DbValue::DbValue(sisl::Blob const& src, std::optional< size_t > fixed_size) :
        view_{src}, fixed_size_{fixed_size} {}

sisl::Blob DbValue::serialize() const { return view_; }

uint32_t DbValue::serialized_size() const { return fixed_size_ ? to_u32(*fixed_size_) : size(); }

void DbValue::deserialize(sisl::Blob const& b, bool copy) {
    if (copy) {
        owner_ = alloc_owned(b);
        view_ = sisl::Blob{owner_->cbytes(), owner_->size()};
    } else {
        owner_.reset();
        view_ = b;
    }
}

std::string DbValue::to_string() const {
    if (fixed_size_) {
        return fmt::format("DbValue(fixed {} bytes)", *fixed_size_);
    }
    return fmt::format("DbValue(var {} bytes)", size());
}

} // namespace homedb
