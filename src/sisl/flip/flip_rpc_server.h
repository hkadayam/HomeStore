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
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Remote-control grpc endpoint for flip — NOT CURRENTLY BUILT.
//
// This file is preserved in source as the reference implementation for when grpc is brought back into the build.
// The flip stack moved off protobuf onto flatbuffer (see proto/flip_spec.fbs); reviving this surface needs:
//   1. Add grpc + (re-add) protobuf — or rewrite the service stubs to use flatbuffer-on-the-wire (flatc --grpc).
//   2. Replace FlipSpec / FlipResponse / FlipNameRequest etc. (protobuf types) with FlipSpecT (flatbuffer object
//      API) plus a serializer at the wire boundary.
//   3. Add this header + flip_rpc_server.cpp back to sisl_flip's CMakeLists, then remove the #if 0 below.
//
// Intentionally left as #if 0 (rather than deleted) so the next person doesn't have to dig commit history to find
// the prior shape of the rpc surface.
// ─────────────────────────────────────────────────────────────────────────────

#if 0
#include <string>

#include <grpcpp/grpcpp.h>

#include "proto/flip_spec.pb.h"
#include "proto/flip_server.grpc.pb.h"

namespace flip {
class FlipRPCServer final : public FlipServer::Service {
public:
    FlipRPCServer() = default;
    grpc::Status InjectFault(grpc::ServerContext* context, const FlipSpec* request, FlipResponse* response) override;
    grpc::Status GetFaults(grpc::ServerContext* context, const FlipNameRequest* request,
                           FlipListResponse* response) override;
    grpc::Status RemoveFault(grpc::ServerContext*, const FlipRemoveRequest* request,
                             FlipRemoveResponse* response) override;
};

// Launch / shut down a remote-control grpc endpoint that mutates flip::Flip::instance().
void start_flip_rpc_server(const std::string& addr = "0.0.0.0:50051");
void stop_flip_rpc_server();

} // namespace flip
#endif
