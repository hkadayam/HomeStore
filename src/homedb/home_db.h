#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/async.h"
#include "common/defs.h"

namespace homedb {

// ── DeviceSpec ───────────────────────────────────────────────────────────────────────────────────────────────────
// One physical device HomeDB will bind to.  size_bytes == 0 asks the underlying store to probe the actual
// device size (only useful for real block devices; test files should set an explicit size).

struct DeviceSpec {
    std::string path;
    uint64_t size_bytes{0};
};

// ── HomeDB ───────────────────────────────────────────────────────────────────────────────────────────────────────
// Process-level lifecycle wrapper.  start() brings the underlying store up (format on fresh devices, load on
// existing — chosen from device state, not the caller).  shutdown() takes it back down.  All user-facing
// state (Databases, Tables) is created explicitly via Database::open() after start() returns.  The public
// surface intentionally hides the underlying store type from callers.

class HomeDB {
public:
    /// Boot from the supplied devices.  Fresh → format + go live.  Existing → load; Database::open() drives
    /// the subsequent replay on the recovery path.
    static Async< shared< HomeDB > > start(std::vector< DeviceSpec > devices);

    HomeDB(HomeDB const&) = delete;
    HomeDB& operator=(HomeDB const&) = delete;
    HomeDB(HomeDB&&) = delete;
    HomeDB& operator=(HomeDB&&) = delete;
    ~HomeDB() = default;

    /// Tear down.  Caller must have destroyed all Databases first (their Tables unregister from Journal in
    /// ~Database).
    Async< void > shutdown();

private:
    HomeDB() = default;
};

} // namespace homedb
