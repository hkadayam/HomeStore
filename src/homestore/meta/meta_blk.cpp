/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

#include <cstring>
#include <stdexcept>

#include "meta/meta_blk.hpp"
#include "meta/meta_client.hpp"
#include "device/virtual_dev.h" // VirtualDev

namespace homestore {

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk Public APIs
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlk::write_data(const IOBuffer& buf, VirtualDev& vdev) {
    const BlkId old_ovf = header().overflow_bid;

    if (buf.size() <= max_inline_data_size()) {
        std::memcpy(data_slice().data(), buf.data(), buf.size());
        header().overflow_bid = BlkId{};
    } else {
        // Allocate contiguous overflow blocks.
        const size_t blk_sz = vdev.block_size();
        const auto n_ovf = static_cast< blk_count_t >((buf.size() + blk_sz - 1) / blk_sz);

        blk_alloc_hints hints{};
        BlkId ovf_bid{};
        BlkAllocStatus st = vdev.alloc_contiguous_blks(n_ovf, hints, ovf_bid);
        if (st != BlkAllocStatus::SUCCESS) { throw std::runtime_error{"MetaBlk::write_data: overflow alloc failed"}; }

        co_await vdev.write(buf, ovf_bid);
        header().overflow_bid = ovf_bid;
    }

    header().data_size = static_cast< uint32_t >(buf.size());
    header().data_crc = crc32_ieee(0, buf.data(), buf.size());

    // Write this block (header + inline data) to disk.
    co_await vdev.write(buffer, blkid);

    // Free the old overflow block now that new data is safely on disk.
    if (old_ovf.is_valid()) { vdev.free_blk(old_ovf); }
}

folly::coro::Task< IOBuffer > MetaBlk::read_data(VirtualDev& vdev) const {
    const MetaBlkHeader& hdr = header();
    const size_t data_sz = hdr.data_size;

    IOBuffer out{data_sz};

    if (hdr.data_size <= max_inline_data_size()) {
        // Inline: copy directly from the cached buffer.
        std::memcpy(out.data(), data_slice().data(), data_sz);
    } else {
        // Overflow: read from the overflow blocks.
        auto [err, out2] = co_await vdev.read(std::move(out), hdr.overflow_bid);
        if (err) { throw std::system_error{err, "MetaBlk::read_data: overflow read failed"}; }
        co_return std::move(out2);
    }

    co_return out;
}

folly::coro::Task< void > MetaBlk::free(VirtualDev& vdev) {
    if (header().overflow_bid.is_valid()) { vdev.free_blk(header().overflow_bid); }
    vdev.free_blk(blkid);
    co_return;
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk::update_next_bid  (private — called only by MetaClient)
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< void > MetaBlk::update_next_bid(BlkId next, VirtualDev& vdev) {
    header().next_bid = next;
    co_await vdev.write(buffer, blkid);
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkWrapper Public APIs
// ──────────────────────────────────────────────────────────────────────────────
folly::coro::Task< MetaBlkWrapper > MetaBlkWrapper::create(shared< MetaClient > client, std::string_view name,
                                                           std::optional< size_t > estimated_data_size) {
    MetaBlk blk = co_await client->create_meta_blk(name, estimated_data_size);
    MetaBlkWrapper w;
    w.meta_blk_ = std::move(blk);
    w.client_ = std::move(client);
    co_return w;
}

folly::coro::Task< void > MetaBlkWrapper::write(const uint8_t* data, size_t len) {
    IOBuffer buf{len};
    std::memcpy(buf.data(), data, len);
    co_await client_->write_meta_blk(meta_blk_.clone(), buf);
}

folly::coro::Task< IOBuffer > MetaBlkWrapper::read() { co_return co_await client_->read_meta_blk(meta_blk_); }

} // namespace homestore
