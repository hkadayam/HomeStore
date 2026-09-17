#include "homedb/home_db.h"

#include "sisl/logging/logging.h"

#include "homestore/base/homestore_decl.h"
#include "homestore/homestore.h"

namespace homedb {

static homestore::InputParams to_input_params(std::vector< DeviceSpec > const& devices) {
    homestore::InputParams ip{};
    ip.devices.reserve(devices.size());
    for (auto const& d : devices) {
        ip.devices.emplace_back(d.path, homestore::HSDevType::Data, d.size_bytes);
    }
    ip.data_open_flags = homestore::IOFlag::DIRECT_IO;
    ip.fast_open_flags = homestore::IOFlag::DIRECT_IO;
    return ip;
}

Async< shared< HomeDB > > HomeDB::start(std::vector< DeviceSpec > devices) {
    auto* hs = homestore::HomeStore::instance();
    bool const first = co_await hs->start(to_input_params(devices));

    if (first) {
        co_await hs->format(); // format() takes HomeStore live; no replay path on fresh boot.
    } else {
        co_await hs->load(); // load() reconstructs managers; Database::open drives replay for the recovery path.
    }

    LOGINFO("HomeDB: start complete (first_time={})", first);
    co_return shared< HomeDB >{new HomeDB{}};
}

Async< void > HomeDB::shutdown() {
    co_await homestore::HomeStore::instance()->shutdown();
    homestore::HomeStore::reset_instance();
    LOGINFO("HomeDB: shutdown complete");
    co_return;
}

} // namespace homedb
