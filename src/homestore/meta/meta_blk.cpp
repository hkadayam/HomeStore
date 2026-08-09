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
    const bool overflow = data && (data->size() > max_inline_data_size());

    // Overflow payload goes to freshly-allocated blocks straight from the caller's buffer — zero copy; the
    // caller's freeze-until-return contract keeps those bytes stable.  Done before the header block below so a
    // crash in between leaves the previous generation fully intact.  The crc over a payload buffer is likewise
    // computed outside the holder lock: those bytes are the caller's, not the holder's.
    BlkId ovf_bid{};
    uint32_t crc = 0;
    if (overflow) {
        const size_t blk_sz = vdev.block_size();
        const auto n_ovf = static_cast< blk_count_t >((data->size() + blk_sz - 1) / blk_sz);
        blk_alloc_hints hints{};
        BlkAllocStatus st = vdev.alloc_contiguous_blks(n_ovf, hints, ovf_bid);
        if (st != BlkAllocStatus::SUCCESS) {
            throw std::runtime_error{"MetaBlk::write_data: overflow alloc failed"};
        }
        co_await vdev.write(*data, ovf_bid);
        META_LOG(DEBUG, "write_data: name={} overflow data_size={} ovf_blk_num={} ovf_nblks={}", name(), data->size(),
                 ovf_bid.blk_num(), ovf_bid.blk_count());
    }
    if (data) {
        crc = crc32_ieee(0, data->cbytes(), data->size());
    }

    // Stamp under the lock: a mutation lands entirely before or entirely after this moment, never across it.
    // From here `wbuf` is immutable for the IO duration — a mutate_buf() copy-swaps onto a new generation.
    sisl::IoBufShared wbuf;
    {
        std::lock_guard lk(holder_->mtx); // acquiring it waits out a live mutate guard (µs)
        HS_DBG_ASSERT(!holder_->io_in_flight, "meta_blk {}: two concurrent writes of one block", name());
        if (overflow) {
            header().overflow_bid = ovf_bid;
            header().data_size = to_u32(data->size());
        } else if (data) {
            std::memcpy(mutable_inline_data(), data->cbytes(), data->size());
            header().overflow_bid = BlkId{};
            header().data_size = to_u32(data->size());
        } else {
            crc = crc32_ieee(0, inline_data(), header().data_size); // payload-less: persist what we hold
        }
        header().data_crc = crc;
        holder_->io_in_flight = true;
        wbuf = holder_->buffer;
    }
    META_LOG(DEBUG, "write_data: name={} data_size={} blk_num={}{}", name(), header().data_size,
             holder_->blkid.blk_num(), data ? "" : " (payload-less)");

    // Write the single cached block (header + inline data) to disk.
    co_await vdev.write(*wbuf, holder_->blkid);

    {
        std::lock_guard lk(holder_->mtx);
        holder_->io_in_flight = false;
    }

    // Free the old overflow block now that new data is safely on disk.
    if (old_ovf.is_valid()) {
        vdev.free_blk(old_ovf);
    }
}

Async< sisl::IoBufView > MetaBlk::read_data(VirtualDev& vdev) const {
    const uint32_t data_sz = header().data_size;

    if (!header().overflow_bid.is_valid()) {
        META_LOG(DEBUG, "read_data: name={} inline data_size={} blk_num={}", name(), data_sz, holder_->blkid.blk_num());
        sisl::IoBufView view{holder_->buffer, to_u32(MetaBlkHeader::SIZE), data_sz};
        if (crc32_ieee(0, view.cbytes(), data_sz) != header().data_crc) {
            throw std::runtime_error{"MetaBlk::read_data: payload crc mismatch (inline) name=" + name()};
        }
        co_return view;
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
    if (crc32_ieee(0, out->cbytes(), data_sz) != header().data_crc) {
        throw std::runtime_error{"MetaBlk::read_data: payload crc mismatch (overflow) name=" + name()};
    }
    co_return sisl::IoBufView{std::move(out)};
}

Async< void > MetaBlk::free(VirtualDev& vdev) {
    if (header().overflow_bid.is_valid()) {
        vdev.free_blk(header().overflow_bid);
    }
    vdev.free_blk(holder_->blkid);
    co_return;
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlk::update_next_bid  (private — called only by MetaClient)
// ──────────────────────────────────────────────────────────────────────────────
Async< void > MetaBlk::update_next_bid(BlkId next, VirtualDev& vdev) {
    // Chain bookkeeping rewrites this block's header while its consumer may be mutating the payload — same
    // stamp discipline as write_data: stamp under the lock, write frozen bytes, mutations copy-swap meanwhile.
    sisl::IoBufShared wbuf;
    {
        std::lock_guard lk(holder_->mtx);
        HS_DBG_ASSERT(!holder_->io_in_flight, "meta_blk {}: two concurrent writes of one block", name());
        header().next_bid = next;
        if (!header().overflow_bid.is_valid()) {
            // An Exclusive block's inline payload may have been guard-mutated since its last write_data — restamp
            // the crc so this chain rewrite never persists mutated bytes under a stale checksum.
            header().data_crc = crc32_ieee(0, inline_data(), header().data_size);
        }
        holder_->io_in_flight = true;
        wbuf = holder_->buffer;
    }
    META_LOG(DEBUG, "update_next_bid: name={} blk_num={} set next_blk_num={}", name(), holder_->blkid.blk_num(),
             next.is_valid() ? next.blk_num() : 0);
    co_await vdev.write(*wbuf, holder_->blkid);
    {
        std::lock_guard lk(holder_->mtx);
        holder_->io_in_flight = false;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// MetaBlkWrapper Public APIs
// ──────────────────────────────────────────────────────────────────────────────
Async< MetaBlkWrapper > MetaBlkWrapper::create(shared< MetaClient > client, std::string_view name,
                                               std::optional< size_t > estimated_data_size, MetaBlkOwnership owner) {
    MetaBlk blk = co_await client->create_meta_blk(name, estimated_data_size, owner);
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

Async< void > MetaBlkWrapper::write() {
    co_await client_->write_meta_blk(meta_blk_);
}

Async< sisl::IoBufView > MetaBlkWrapper::read() {
    co_return co_await client_->read_meta_blk(meta_blk_);
}

Async< void > MetaBlkWrapper::destroy() {
    co_await client_->remove_meta_blk(meta_blk_);
}

} // namespace homestore