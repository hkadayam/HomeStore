#include "folly_rpc_client_factory.h"
#include "folly_rpc_client.h"

#include <folly/hash/Hash.h>

#include "iomanager/iomanager.h"

namespace homestore::replication {

namespace {
inline std::string make_key(std::string const& host, uint16_t port) {
    return fmt::format("{}:{}", host, port);
}
} // namespace

FollyRpcClientFactory::FollyRpcClientFactory(folly::Executor* slow_executor) :
        slow_executor_(slow_executor), outbound_([]() { return PerReactorState{}; }) {
}

FollyRpcClientFactory::~FollyRpcClientFactory() = default;

nuraft::ptr< nuraft::rpc_client > FollyRpcClientFactory::create_client(std::string const& endpoint) {
    // Parse "host:port"
    auto colon = endpoint.find_last_of(':');
    std::string host = (colon == std::string::npos) ? endpoint : endpoint.substr(0, colon);
    uint16_t port = (colon == std::string::npos) ? 0 : static_cast< uint16_t >(std::stoul(endpoint.substr(colon + 1)));
    uint64_t id = client_id_seq_.fetch_add(1, std::memory_order_relaxed);
    return nuraft::cs_new< FollyRpcClient >(this, std::move(host), port, id);
}

PeerOutboundSocket& FollyRpcClientFactory::get_or_open_outbound(std::string const& host, uint16_t port) {
    auto& state = outbound_.get();
    auto key = make_key(host, port);
    auto it = state.by_endpoint.find(key);
    if (it == state.by_endpoint.end()) {
        auto* eb = iomanager::iomgr().reactor_for(iomanager::iomgr().current_reactor_id());
        auto sock = std::make_unique< PeerOutboundSocket >(eb, host, port, slow_executor_);
        it = state.by_endpoint.emplace(std::move(key), std::move(sock)).first;
    }
    return *it->second;
}

size_t FollyRpcClientFactory::pick_reactor_for_cold_path(std::string const& host, uint16_t port) const {
    auto key = make_key(host, port);
    return folly::hash::fnv64(key) % iomanager::iomgr().num_reactors();
}

} // namespace homestore::replication
