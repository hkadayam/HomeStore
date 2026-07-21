/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "sisl/fds/bitset.h"
#include "sisl/fds/sparse_vector.h"
#include "sisl/fds/utils.h"
#include "common/defs.h"
#include "homestore/device/device_decl.h"
#include "homestore/base/crc.h"
#include "homestore/base/hs_compile_config.h"

#ifdef _PRERELEASE
#include "sisl/flip/flip.h"
#endif

// Super blk format
//  _______________________________________________________________________________________________________________
//  |        |<--------------Vdev Area--------------->|  <---------------------Chunk Area--------------->|         |
//  | First  | Vdev Slot | Vdev[0]| Vdev[1]| .. |V[N] | Chunk Slot | Chunk[0] | Chunk[1]| .. | Chunk[M]  |Reserved |
//  | Block  | Bitmap    | Info   | Info   |    |Info | Bitmap     | Info     | Info    |    | Info      | Space   |
//  |________|___________|________|________|____|_____|____________|__________|_________|____|___________|_________|
//
//  where:
//    N = MAX_VDEVS_IN_SYSTEM
//    M = max_chunks_in_pdev(dinfo)

namespace homestore {

#pragma pack(1)
struct DiskAttr {
    static constexpr uint32_t DEFAULT_ALIGN_SIZE = 512;

    // all fields in this structure mirror drive attributes (page sizes, alignment, streams)
    uint32_t phys_page_size{0};              // Physical page size of flash ssd/nvme. This is optimal size to do IO
    uint32_t align_size{DEFAULT_ALIGN_SIZE}; // size alignment supported by drives/kernel
    uint32_t atomic_phys_page_size{0};       // atomic page size of the drive_sync_write_count
    uint32_t num_streams{0};

    DiskAttr() = default;
    DiskAttr(uint32_t pps, uint32_t as, uint32_t apps, uint32_t ns) :
            phys_page_size{pps}, align_size{as}, atomic_phys_page_size{apps}, num_streams{ns} {}

    bool is_valid() const {
        return is_page_valid(phys_page_size) && is_page_valid(align_size) && is_page_valid(atomic_phys_page_size);
    }

    bool is_page_valid(uint32_t page_size) const {
        return (page_size == 0 || (page_size & (page_size - 1)) != 0) ? false : true;
    }

    std::string to_string() const {
        return fmt::format("phys_page_size={}, align_size={}, atomic_phys_page_size={}, num_streams={}",
                           in_bytes(phys_page_size), in_bytes(align_size), in_bytes(atomic_phys_page_size),
                           num_streams);
    }
};

struct FirstBlockHeader {
    static constexpr const char* PRODUCT_NAME{"HomeStore4x"};
    static constexpr size_t s_product_name_size{64};
    static constexpr uint32_t CURRENT_SUPERBLOCK_VERSION{4};

public:
    uint64_t gen_number{0};                   // Generation count of this structure
    uint32_t version{0};                      // Version Id of this structure
    char product_name[s_product_name_size]{}; // Product name

    uint32_t num_pdevs{0};         // Total number of pdevs homestore is being created on
    uint32_t max_vdevs{0};         // Max VDevs possible, this cannot be changed post formatting
    uint32_t max_system_chunks{0}; // Max Chunks possible, this cannot be changed post formatting
    Uuid system_uuid;

public:
    const char* get_product_name() const { return product_name; }
    uint32_t get_version() const { return version; }
    Uuid get_system_uuid() const { return system_uuid; }
    std::string get_system_uuid_str() const { return boost::uuids::to_string(system_uuid); };

    std::string to_string() const {
        auto str = fmt::format("gen_number={}, version={}, product_name={} system_uuid={}", gen_number, get_version(),
                               get_product_name(), get_system_uuid_str());
        return str;
    }
};

struct PDevInfoHeader {
public:
    uint64_t data_offset{0};         // Offset within pdev where data starts
    uint64_t size{0};                // Total pdev size
    uint32_t pdev_id{0};             // Device ID for this store instance.
    uint32_t max_pdev_chunks{0};     // Max chunks in this pdev possible
    DiskAttr dev_attr;               // Attributes homestore expects from all the devices.
    uint8_t mirror_super_block{0x0}; // Have we mirrored the super block on head/tail
    Uuid system_uuid;                // Current system uuid stamp to protect from device exchange

public:
    std::string to_string() const {
        auto str =
            fmt::format("data_offset={}, size={}, pdev_id={} max_pdev_chunks={} dev_attr=[{}] mirror_super_block?={}",
                        in_bytes(data_offset), in_bytes(size), pdev_id, max_pdev_chunks, dev_attr.to_string(),
                        (mirror_super_block == 0x00) ? "false" : "true");
        return str;
    }

    std::string get_system_uuid_str() const { return boost::uuids::to_string(system_uuid); };
};

struct FirstBlock {
    static constexpr uint32_t s_atomic_fb_size{512};       // increase 512 to actual size if in the future FirstBlock
                                                           // can be larger;
    static constexpr uint32_t s_io_fb_size{4096};          // This is the size we do IO on, with padding
    static constexpr uint32_t HOMESTORE_MAGIC{0xABBECDCD}; // Magic written as first bytes on each device

public:
    uint64_t magic{0};                // Header magic expected to be at the top of block
    uint32_t checksum{0};             // Checksum of the entire first block (excluding this field)
    uint32_t formatting_done : 1 {0}; // Has formatting completed yet
    uint32_t reserved : 31 {0};
    FirstBlockHeader hdr;         // Information about the entire system
    PDevInfoHeader this_pdev_hdr; // Information about the current pdev

public:
    uint64_t get_magic() const { return magic; }

    bool is_valid() const {
        return (
            (magic == HOMESTORE_MAGIC) &&
            (std::string(hdr.product_name) == std::string(FirstBlockHeader::PRODUCT_NAME) && (formatting_done != 0x0)));
    }

    std::string to_string() const {
        auto str = fmt::format("magic={:#x}, checksum={}, first_blk_header=[{}], this_pdev_info=[{}]", get_magic(),
                               checksum, hdr.to_string(), this_pdev_hdr.to_string());
        return str;
    }
};
#pragma pack()
static_assert(sizeof(FirstBlock) <= FirstBlock::s_atomic_fb_size);

// ── VDevInfo ──────────────────────────────────────────────────────────────────
// On-disk structure for virtual device metadata.
//
// 512   = VDevInfo::SIZE
#pragma pack(1)
struct VDevInfo {
    static constexpr size_t SIZE = 512;
    static constexpr size_t USER_PRIVATE_SIZE = 256;
    static constexpr size_t MAX_NAME_LEN = 64;

    uint32_t vdev_id{0};                       //   0
    uint32_t num_mirrors{0};                   //   4
    uint32_t blk_size{0};                      //   8
    uint32_t initial_chunk_size{0};            //  12  Size used for chunks created at vdev init time and the default
                                               //      size for expand() calls that don't override.  Per-chunk size
                                               //      may differ for chunks added later; check chunk->size().
    uint8_t slot_allocated{0};                 //  16
    uint8_t failed{0};                         //  17
    uint8_t hs_dev_type{0};                    //  18  (HSDevType as u8)
    uint8_t multi_pdev_choice{0};              //  19  (MultiPDevOpts as u8)
    char name[MAX_NAME_LEN]{};                 //  20
    uint16_t checksum{0};                      //  84
    uint8_t alloc_type{0};                     //  86  (BlkAllocatorType as u8)
    uint8_t chunk_sel_type{0};                 //  87  (ChunkSelectorType as u8)
    uint8_t persist_blk_alloced{1};            //  88
    uint8_t padding[167]{};                    //  89
    uint8_t user_private[USER_PRIVATE_SIZE]{}; // 256

    // ── Accessors ──────────────────────────────────────────────────────────
    bool is_allocated() const { return slot_allocated == 0x01; }
    void set_allocated() { slot_allocated = 0x01; }
    void set_free() { slot_allocated = 0x00; }

    bool is_failed() const { return failed == 0x01; }

    void set_name(const std::string& n) {
        std::strncpy(name, n.c_str(), MAX_NAME_LEN - 1);
        name[MAX_NAME_LEN - 1] = '\0';
    }
    std::string get_name() const { return std::string{name}; }

    void compute_checksum() {
        checksum = 0;
        checksum = crc16_t10dif(hs_init_crc_16, reinterpret_cast< const unsigned char* >(this), sizeof(VDevInfo));
    }

    const uint8_t* to_bytes() const { return reinterpret_cast< const uint8_t* >(this); }

    /// Byte offset of this vdev's record within the superblock area on any pdev.
    static uint64_t vdev_info_offset(uint32_t vdev_id);
};
#pragma pack()
static_assert(sizeof(VDevInfo) == VDevInfo::SIZE, "VDevInfo size mismatch");

// ── ChunkInfo ─────────────────────────────────────────────────────────────────
// On-disk structure for chunk metadata.
//
// 512   = ChunkInfo::SIZE
#pragma pack(1)
struct ChunkInfo {
    static constexpr size_t SIZE = 512;
    static constexpr size_t USER_PRIVATE_SIZE = 128;
    static constexpr size_t SELECTOR_PRIVATE_SIZE = 64;

    uint64_t chunk_start_offset{0};                          //   0: start offset within pdev
    uint64_t chunk_size{0};                                  //   8: size of this chunk
    uint32_t vdev_id{0};                                     //  16: owning vdev (UINT32_MAX = free)
    uint32_t chunk_id{0};                                    //  20: system-wide unique chunk id
    uint64_t chunk_vdev_order{0};                            //  24: sequential creation order in vdev
    uint64_t stream_id{0};                                   //  32: stream id (0 = unassigned/default)
    uint32_t checksum{0};                                    //  40: CRC32 of entire ChunkInfo
    uint8_t chunk_allocated{0x00};                           //  44: 0x01 = allocated, 0x00 = free
    uint8_t padding2[3]{};                                   //  45: align to 4-byte boundary
    uint8_t padding[272]{};                                  //  48
    uint8_t chunk_selector_private[SELECTOR_PRIVATE_SIZE]{}; // 320
    uint8_t user_private[USER_PRIVATE_SIZE]{};               // 384

    // ── Accessors ─────────────────────────────────────────────────────────────
    bool is_allocated() const { return chunk_allocated != 0x00; }
    void set_allocated() { chunk_allocated = 0x01; }
    void set_free() { chunk_allocated = 0x00; }

    bool has_stream() const { return stream_id != 0; }
    uint64_t get_stream_id() const { return stream_id; }
    void set_stream_id(uint64_t sid) { stream_id = sid; }

    void set_selector_private(const uint8_t* data, size_t len) {
        if (data && len > 0) {
            std::memcpy(chunk_selector_private, data, std::min(len, SELECTOR_PRIVATE_SIZE));
        }
    }

    void set_user_private(const uint8_t* data, size_t len) {
        if (data && len > 0) {
            std::memcpy(user_private, data, std::min(len, USER_PRIVATE_SIZE));
        }
    }

    void compute_checksum() {
        checksum = 0;
        checksum = crc32_ieee(hs_init_crc_32, reinterpret_cast< const unsigned char* >(this), sizeof(ChunkInfo));
    }

    // Raw-byte view of this struct
    const uint8_t* to_bytes() const { return reinterpret_cast< const uint8_t* >(this); }
};
#pragma pack()
static_assert(sizeof(ChunkInfo) == ChunkInfo::SIZE, "ChunkInfo size mismatch");

/////////////// Overarching super block information ////////////////
class HSSuperBlk {
public:
    // System-wide vdev / chunk / min-chunk-size limits live in hs_compile_config.h — the single registry
    // of compile-time restrictions (MAX_VDEVS_IN_SYSTEM, MAX_CHUNKS_IN_SYSTEM, MIN_CHUNK_SIZE_{DATA,FAST}_DEVICE).
    static constexpr uint64_t EXTRA_SB_SIZE_FOR_DATA_DEVICE = 8 * 1024 * 1024;
    static constexpr uint64_t EXTRA_SB_SIZE_FOR_FAST_DEVICE = 1 * 1024 * 1024;

    static constexpr uint32_t first_block_offset() { return 0; } // Offset in physical device we can use for first block
    static constexpr uint32_t first_block_size() { return FirstBlock::s_io_fb_size; }

    // Vdev slot bitmap: one bit per possible vdev slot
    static uint64_t vdev_slot_bitmap_size() {
        return sisl::Bitset::serialized_size(to_u64(MAX_VDEVS_IN_SYSTEM), DiskAttr::DEFAULT_ALIGN_SIZE);
    }

    // Total vdev area: slot bitmap + one VDevInfo record per slot
    static uint64_t vdev_super_block_size() {
        return vdev_slot_bitmap_size() + to_u64(MAX_VDEVS_IN_SYSTEM) * VDevInfo::SIZE;
    }

    // Chunk slot bitmap: one bit per possible chunk in this pdev
    static uint64_t chunk_info_bitmap_size(const DevInfo& dinfo) {
        return sisl::Bitset::serialized_size(to_u64(max_chunks_in_pdev(dinfo)), DiskAttr::DEFAULT_ALIGN_SIZE);
    }

    // Total chunk area: slot bitmap + one ChunkInfo record per slot
    static uint64_t chunk_super_block_size(const DevInfo& dinfo) {
        return chunk_info_bitmap_size(dinfo) + to_u64(max_chunks_in_pdev(dinfo)) * ChunkInfo::SIZE;
    }

    static uint64_t total_size(const DevInfo& dinfo) { return total_used_size(dinfo) + future_padding_size(dinfo); }
    static uint64_t total_used_size(const DevInfo& dinfo) {
        return first_block_size() + vdev_super_block_size() + chunk_super_block_size(dinfo);
    }
    static uint64_t vdev_sb_offset() { return first_block_offset() + first_block_size(); }
    static uint64_t chunk_sb_offset() { return vdev_sb_offset() + vdev_super_block_size(); }

    static uint64_t future_padding_size(const DevInfo& dinfo) {
        return (dinfo.dev_type == HSDevType::Fast) ? EXTRA_SB_SIZE_FOR_FAST_DEVICE : EXTRA_SB_SIZE_FOR_DATA_DEVICE;
    }
    static uint32_t max_chunks_in_pdev(const DevInfo& dinfo) {
        // Do not round up , for a device with 128MB and min_chunk_size = 16MB, we should get 8 chunks
        // for a device with 100MB and min_chunk_size = 16MB, we should get 6 chunks, not 7.
        return dinfo.dev_size / min_chunk_size(dinfo.dev_type);
    }
    static uint32_t min_chunk_size(HSDevType dtype) {
        uint64_t min_chunk_size = (dtype == HSDevType::Fast) ? MIN_CHUNK_SIZE_FAST_DEVICE : MIN_CHUNK_SIZE_DATA_DEVICE;
#ifdef _PRERELEASE
        auto chunk_size = flip::Flip::instance().get_test_flip< long >("set_minimum_chunk_size");
        if (chunk_size) {
            min_chunk_size = chunk_size.value();
        }
#endif
        return min_chunk_size;
    }
};

} // namespace homestore
