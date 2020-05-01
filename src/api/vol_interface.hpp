#pragma once

/* NOTE: This file exports interface required to access homeblocks. we should try to avoid including any
 * homestore/homeblocks related hpp file.
 */
#include "engine/common/homestore_header.hpp"
#include "engine/common/homestore_config.hpp"
#include <functional>
#include <vector>
#include <memory>
#include <engine/common/error.h>
#include <iomgr/iomgr.hpp>
#include <boost/intrusive_ptr.hpp>
#include <cassert>
#include <sds_logging/logging.h>
#include <mutex>
#include <utility/atomic_counter.hpp>
//#include <fds/utils.hpp>
#include <fds/utils.hpp>
#include <utility/obj_life_counter.hpp>
#include <atomic>
#include <boost/optional.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <sstream>
#include <string>
#include <iostream>
#include <utility/enum.hpp>
#include <variant>

namespace homestore {
class Volume;
class Snapshot;
class BlkBuffer;
void intrusive_ptr_add_ref(BlkBuffer* buf);
void intrusive_ptr_release(BlkBuffer* buf);

class VolInterface;
struct init_params;
class VolInterfaceImpl {
public:
    static VolInterface* init(const init_params& cfg, bool force_reinit);
    static boost::intrusive_ptr< VolInterface > safe_instance();
    static VolInterface* raw_instance();
};

struct buf_info {
    uint64_t size;
    int offset;
    boost::intrusive_ptr< BlkBuffer > buf;

    buf_info(uint64_t sz, int off, boost::intrusive_ptr< BlkBuffer >& bbuf) : size(sz), offset(off), buf(bbuf) {}
};

struct _counter_generator {
    static _counter_generator& instance() {
        static _counter_generator inst;
        return inst;
    }

    _counter_generator() : request_id_counter(0) {}
    uint64_t next_request_id() { return request_id_counter.fetch_add(1, std::memory_order_relaxed); }
    std::atomic< uint64_t > request_id_counter;
};
#define counter_generator _counter_generator::instance()

struct volume_req;
struct vol_interface_req : public sisl::ObjLifeCounter< vol_interface_req > {
    std::shared_ptr< Volume > vol_instance;
    std::vector< buf_info > read_buf_list;
    void* write_buf = nullptr;
    std::error_condition err = no_error;
    uint64_t request_id;
    sisl::atomic_counter< int > refcount;
    std::atomic< bool > is_fail_completed = false;
    uint64_t lba;
    uint32_t nlbas;
    bool is_read = true;
    bool sync = false;
    bool part_of_batch = false;
    void* cookie;

    friend void intrusive_ptr_add_ref(vol_interface_req* req) { req->refcount.increment(1); }

    friend void intrusive_ptr_release(vol_interface_req* req) {
        if (req->refcount.decrement_testz()) { req->free_yourself(); }
    }

    void inc_ref_cnt() { intrusive_ptr_add_ref(this); }

    /* Set the error with error code,
     * Returns
     * true: if it is able to set the error
     * false: if request is already completed
     */
    bool set_error(const std::error_condition& ec) {
        bool expected_val = false;
        if (is_fail_completed.compare_exchange_strong(expected_val, true, std::memory_order_acq_rel)) {
            err = ec;
            return true;
        } else {
            return false;
        }
    }

    std::error_condition get_status() const { return err; }

public:
    vol_interface_req(void* wbuf, uint64_t lba, uint32_t nlbas, bool is_sync = false);
    virtual ~vol_interface_req();
    virtual void free_yourself() { delete this; }
};

typedef boost::intrusive_ptr< vol_interface_req > vol_interface_req_ptr;

ENUM(vol_state, uint32_t,
     ONLINE,     // Online state after loading
     FAILED,     // It moved to offline only when it find vdev in failed state during boot
     OFFLINE,    // Either AM can move it to offline or internally HS can move it offline if there are error on a disk
     DEGRADED,   // If a data of a volume in a failed state is deleted. We delete the data if we found any volume in a
                 // failed state during boot.
     MOUNTING,   // Process of mounting
     DESTROYING, // Marked to this state, while actual volume is deleted
     UNINITED,   // Initial state when volume is brought up
     DESTROYED   // destroyed
);

typedef std::function< void(const vol_interface_req_ptr& req) > io_single_comp_callback;
typedef std::function< void(const std::vector< vol_interface_req_ptr >& reqs) > io_batch_comp_callback;
typedef std::function< void(bool success) > shutdown_comp_callback;
typedef std::function< void(int n_completions) > end_of_batch_callback;
typedef std::variant< io_single_comp_callback, io_batch_comp_callback > io_comp_callback;

struct vol_params {
    uint64_t page_size;
    uint64_t size;
    boost::uuids::uuid uuid;
    io_comp_callback io_comp_cb;
#define VOL_NAME_SIZE 100
    char vol_name[VOL_NAME_SIZE];

    std::string to_string() const {
        std::stringstream ss;
        ss << "page_size=" << page_size << ",size=" << size << ",vol_name=" << vol_name
           << ",uuid=" << boost::lexical_cast< std::string >(uuid);
        return ss.str();
    }
};

struct out_params {
    uint64_t max_io_size; // currently it is 1 MB based on 4k minimum page size
};

typedef std::shared_ptr< Volume > VolumePtr;
typedef std::shared_ptr< Snapshot > SnapshotPtr;

struct init_params : public hs_input_params {
public:
    typedef std::function< void(std::error_condition err, const out_params& params) > init_done_callback;
    typedef std::function< bool(boost::uuids::uuid uuid) > vol_found_callback;
    typedef std::function< void(const VolumePtr& vol, vol_state state) > vol_mounted_callback;
    typedef std::function< void(const VolumePtr& vol, vol_state old_state, vol_state new_state) >
        vol_state_change_callback;

    /* completions callback */
    init_done_callback init_done_cb;
    vol_found_callback vol_found_cb;
    vol_mounted_callback vol_mounted_cb;
    vol_state_change_callback vol_state_change_cb;
    end_of_batch_callback end_of_batch_cb;

public:
    std::string to_string() {
        std::stringstream ss;
        ss << "min_virtual_page_size=" << min_virtual_page_size << ",cache_size=" << cache_size
           << ",disk_init=" << disk_init << ",is_file=" << is_file << ",open_flags =" << open_flags
           << ",number of devices =" << devices.size();
        ss << "device names = ";
        for (uint32_t i = 0; i < devices.size(); ++i) {
            ss << devices[i].dev_names;
            ss << ",";
        }
        ss << "]";
        return ss.str();
    }
    init_params() = default;
};

class VolInterface {
public:
    virtual ~VolInterface() {}
    static bool init(const init_params& cfg, bool force_reinit = false) {
        return (VolInterfaceImpl::init(cfg, force_reinit) != nullptr);
    }
    static VolInterface* get_instance() { return VolInterfaceImpl::raw_instance(); }

    /**
     * @brief Create a vol interface request to do IO using vol interface. This is a helper method and caller are
     * welcome to create a request derived from vol interface request and pass it along instead of calling this method.
     *
     * @param buf - Buffer from write needs to be written to volume. nullptr for read operation
     * @param lba - LBA of the volume
     * @param nlbas - Number of blks to write.
     * @param sync - Is the sync io request or async
     *
     * @return vol_interface_req_ptr
     */
    virtual vol_interface_req_ptr create_vol_interface_req(void* buf, uint64_t lba, uint32_t nlbas,
                                                           bool sync = false) = 0;

    /**
     * @brief Write the data to the volume asynchronously, created from the request. After completion the attached
     * callback function will be called with this req ptr.
     *
     * @param vol Pointer to the volume
     * @param req Request created which contains all the write parameters
     * @param part_of_batch Is this request part of a batch request. If so, implementation can wait for batch_submit
     * call before issuing the writes. IO might already be started or even completed (in case of errors) before
     * batch_sumbit call, so application cannot assume IO will be started only after submit_batch call.
     *
     * @return std::error_condition no_error or error in issuing writes
     */
    virtual std::error_condition write(const VolumePtr& vol, const vol_interface_req_ptr& req,
                                       bool part_of_batch = false) = 0;

    /**
     * @brief Read the data from the volume asynchronously, created from the request. After completion the attached
     * callback function will be called with this req ptr.
     *
     * @param vol Pointer to the volume
     * @param req Request created which contains all the read parameters
     * @param part_of_batch Is this request part of a batch request. If so, implementation can wait for batch_submit
     * call before issuing the reads. IO might already be started or even completed (in case of errors) before
     * batch_sumbit call, so application cannot assume IO will be started only after submit_batch call.
     *
     * @return std::error_condition no_error or error in issuing reads
     */
    virtual std::error_condition read(const VolumePtr& vol, const vol_interface_req_ptr& req,
                                      bool part_of_batch = false) = 0;

    /**
     * @brief Read the data synchronously.
     *
     * @param vol Pointer to the volume
     * @param req Request created which contains all the read parameters
     *
     * @return std::error_condition no_error or error in issuing reads
     */
    virtual std::error_condition sync_read(const VolumePtr& vol, const vol_interface_req_ptr& req) = 0;

    /**
     * @brief Submit the io batch, which is a mandatory method to be called if read/write are issued with part_of_batch
     * is set to true. In those cases, without this method, IOs might not be even issued. No-op if previous io requests
     * are not part of batch.
     */
    virtual void submit_io_batch() = 0;

    virtual const char* get_name(const VolumePtr& vol) = 0;
    virtual uint64_t get_size(const VolumePtr& vol) = 0;
    virtual uint64_t get_page_size(const VolumePtr& vol) = 0;
    virtual boost::uuids::uuid get_uuid(std::shared_ptr< Volume > vol) = 0;
    virtual homeds::blob at_offset(const boost::intrusive_ptr< BlkBuffer >& buf, uint32_t offset) = 0;
    virtual VolumePtr create_volume(const vol_params& params) = 0;
    virtual std::error_condition remove_volume(const boost::uuids::uuid& uuid) = 0;
    virtual VolumePtr lookup_volume(const boost::uuids::uuid& uuid) = 0;
    virtual SnapshotPtr snap_volume(VolumePtr volptr) = 0;

    /* AM should call it in case of recovery or reboot when homestore try to mount the existing volume */
    virtual void attach_vol_completion_cb(const VolumePtr& vol, const io_comp_callback& cb) = 0;
    virtual void attach_end_of_batch_cb(const end_of_batch_callback& cb) = 0;

    virtual bool shutdown(bool force = false) = 0;
    virtual bool trigger_shutdown(const shutdown_comp_callback& shutdown_done_cb, bool force = false) = 0;
    virtual cap_attrs get_system_capacity() = 0;
    virtual bool vol_state_change(const VolumePtr& vol, vol_state new_state) = 0;

    virtual void print_node(const VolumePtr& vol, uint64_t blkid, bool chksum = true) = 0;
    virtual void print_tree(const VolumePtr& vol, bool chksum = true) = 0;
    virtual bool verify_tree(const VolumePtr& vol) = 0;

    /**
     * @brief : fix the btree which is in corrupted state;
     *
     * @param vol : vol that whose mapping btree is to be fixed;
     * @param verify : if true, verify the fixed btree has exact same KVs in the leaf node;
     *
     * @return : true if successfully fixed;
     *           false if not, e.g. when there is corruption in the btree node during fix;
     */
    virtual bool fix_tree(VolumePtr vol, bool verify = false) = 0;
    virtual vol_state get_state(VolumePtr vol) = 0;

#ifdef _PRERELEASE
    virtual void set_io_flip() = 0;
    virtual void set_error_flip() = 0;
#endif
};
} // namespace homestore
