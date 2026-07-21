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
#include "common/async.h"
#include <stdexcept>

#include "common/defs.h"
#include "homestore/meta/meta_blk.h"
#include "homestore/meta/meta_client.h"
#include "homestore/device/virtual_dev.h"

namespace homestore {

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk Public APIs
// ──────────────────────────────────────────────────────────────────────────────
Async< void > MetaBlk::write_data(const sisl::IoBufShared& data, VirtualDev& vdev) {
    const BlkId old_ovf = header().overflow_bid;

    if (data->size() <= max_inline_data_size()) {
        // Inline: copy payload into the cached block after the header.
        std::memcpy(inline_data(), data->cbytes(), data->size());
        header().overflow_bid = BlkId{};
        META_LOG(DEBUG, "write_data: name={} inline data_size={} blk_num={}", name(), data->size(), blkid.blk_num());
    } else {
        // Allocate contiguous overflow blocks for the data.
        const size_t blk_sz = vdev.block_size();
        const auto n_ovf = static_cast< blk_count_t >((data->size() + blk_sz - 1) / blk_sz);
        blk_alloc_hints hints{};
        BlkId ovf_bid{};
        BlkAllocStatus st = vdev.alloc_contiguous_blks(n_ovf, hints, ovf_bid);
        if (st != BlkAllocStatus::SUCCESS) {
            throw std::runtime_error{"MetaBlk::write_data: overflow alloc failed"};
        }
        co_await vdev.write(*data, ovf_bid);
        header().overflow_bid = ovf_bid;
        META_LOG(DEBUG, "write_data: name={} overflow data_size={} ovf_blk_num={} ovf_nblks={}", name(), data->size(),
                 ovf_bid.blk_num(), ovf_bid.blk_count());
    }

    header().data_size = to_u32(data->size());
    header().data_crc = crc32_ieee(0, data->cbytes(), data->size());

    // Write the single cached block (header + inline data) to disk.
    co_await vdev.write(*buffer, blkid);

    // Free the old overflow block now that new data is safely on disk.
    if (old_ovf.is_valid()) {
        vdev.free_blk(old_ovf);
    }
}

Async< sisl::IoBufView > MetaBlk::read_data(VirtualDev& vdev) const {
    const uint32_t data_sz = header().data_size;

    if (!header().overflow_bid.is_valid()) {
        META_LOG(DEBUG, "read_data: name={} inline data_size={} blk_num={}", name(), data_sz, blkid.blk_num());
        co_return sisl::IoBufView{buffer, to_u32(MetaBlkHeader::SIZE), data_sz};
    }

    // Overflow: read from overflow blocks on disk into a new IoBufShared, then wrap as IoBufView.
    const BlkId ovf = header().overflow_bid;
    META_LOG(DEBUG, "read_data: name={} overflow data_size={} ovf_blk_num={} ovf_nblks={}", name(), data_sz,
             ovf.blk_num(), ovf.blk_count());
    auto out = sisl::make_io_buf_shared(data_sz);
    auto err = co_await vdev.read(*out, ovf);
    if (err) {
        throw std::system_error{err, "MetaBlk::read_data: overflow read failed"};
    }
    META_LOG(DEBUG, "read_data: name={} overflow read complete", name());
    co_return sisl::IoBufView{std::move(out)};
}

Async< void > MetaBlk::free(VirtualDev& vdev) {
    if (header().overflow_bid.is_valid()) {
        vdev.free_blk(header().overflow_bid);
    }
    vdev.free_blk(blkid);
    co_return;
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk::update_next_bid  (private — called only by MetaClient)
// ──────────────────────────────────────────────────────────────────────────────
Async< void > MetaBlk::update_next_bid(BlkId next, VirtualDev& vdev) {
    header().next_bid = next;
    // Write the cached block back to disk with the updated header.
    co_await vdev.write(*buffer, blkid);
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkWrapper Public APIs
// ──────────────────────────────────────────────────────────────────────────────
Async< MetaBlkWrapper > MetaBlkWrapper::create(shared< MetaClient > client, std::string_view name,
                                               std::optional< size_t > estimated_data_size) {
    MetaBlk blk = co_await client->create_meta_blk(name, estimated_data_size);
    MetaBlkWrapper w;
    w.meta_blk_ = std::move(blk);
    w.client_ = std::move(client);
    co_return w;
}

Async< void > MetaBlkWrapper::write(const uint8_t* data, size_t len) {
    auto buf = sisl::make_io_buf_shared(to_u32(len));
    std::memcpy(buf->bytes(), data, len);
    co_await client_->write_meta_blk(meta_blk_, buf);
}

Async< sisl::IoBufView > MetaBlkWrapper::read() {
    co_return co_await client_->read_meta_blk(meta_blk_);
}

Async< void > MetaBlkWrapper::destroy() {
    co_await client_->remove_meta_blk(meta_blk_);
}

} // namespace homestore