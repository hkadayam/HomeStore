/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
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
// ─────────────────────────────────────────────────────────────────────────────
// Remote-control grpc endpoint for flip — NOT CURRENTLY BUILT.  See flip_rpc_server.h for the rationale.  When
// grpc is reintroduced, remove the #if 0 wrapper, re-add this .cpp to sisl_flip's CMakeLists, and migrate the
// FlipSpec proto types to FlipSpecT (flatbuffer) at the wire boundary.
// ─────────────────────────────────────────────────────────────────────────────

#if 0
#include <iostream>

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/security/server_credentials.h>

#include "flip_rpc_server.h"
#include "flip.h"

namespace flip {
grpc::Status FlipRPCServer::InjectFault(grpc::ServerContext*, const FlipSpec* request, FlipResponse* response) {
    LOGTRACEMOD(flip, "InjectFault request = {}", request->DebugString());
    flip::Flip::instance().add(*request);
    response->set_success(true);
    return grpc::Status::OK;
}

grpc::Status FlipRPCServer::GetFaults(grpc::ServerContext*, const FlipNameRequest* request,
                                      FlipListResponse* response) {
    LOGTRACEMOD(flip, "GetFaults request = {}", request->DebugString());
    auto resp = request->name().size() ? flip::Flip::instance().get(request->name()) : flip::Flip::instance().get_all();
    for (const auto& r : resp) {
        response->add_infos()->set_info(r);
    }
    LOGTRACEMOD(flip, "GetFaults response = {}", response->DebugString());
    return grpc::Status::OK;
}

grpc::Status FlipRPCServer::RemoveFault(grpc::ServerContext*, const FlipRemoveRequest* request,
                                        FlipRemoveResponse* response) {
    LOGTRACEMOD(flip, "RemoveFault request = {}", request->DebugString());
    response->set_num_removed(flip::Flip::instance().remove(request->name()));
    return grpc::Status::OK;
}

class FlipRPCServiceWrapper : public FlipRPCServer::Service {
public:
    void print_method_names() {
        for (auto i = 0; i < 2; ++i) {
            auto method = (::grpc::internal::RpcServiceMethod*)GetHandler(i);
            if (method) { LOGINFOMOD(flip, "Method name = {}", method->name()); }
        }
    }
};

// File-scope state for the optional remote-control endpoint.  Held here (not in flip.h) so flip.h stays grpc-free.
namespace {
std::unique_ptr< FlipRPCServer > g_flip_server;
std::unique_ptr< grpc::Server > g_grpc_server;
std::unique_ptr< std::thread > g_flip_server_thread;
} // namespace

void start_flip_rpc_server(const std::string& addr) {
    if (g_flip_server) { stop_flip_rpc_server(); }

    g_flip_server = std::make_unique< FlipRPCServer >();
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(static_cast< FlipRPCServer::Service* >(g_flip_server.get()));
    g_grpc_server = builder.BuildAndStart();
    LOGINFOMOD(flip, "Flip GRPC Server listening on {}", addr);
    g_flip_server_thread = std::make_unique< std::thread >([s = g_grpc_server.get()] { s->Wait(); });
}

void stop_flip_rpc_server() {
    if (g_grpc_server) { g_grpc_server->Shutdown(); }
    if (g_flip_server_thread) {
        g_flip_server_thread->join();
        g_flip_server_thread.reset();
    }
    g_grpc_server.reset();
    g_flip_server.reset();
}

} // namespace flip
#endif
