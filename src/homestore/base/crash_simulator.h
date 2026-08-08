#pragma once

#include <functional>

#ifdef SISL_FLIP_ENABLED
#include <csignal>
#include <thread>

#include "sisl/fds/rcu.h"
#include "sisl/flip/flip.h"

#include "homestore/managers.h"
#endif

namespace homestore {

#ifdef SISL_FLIP_ENABLED
// Owns ALL crash-simulation state, entered only through the two free functions below.  A crash marks
// crashed_ — PhysicalDev checks is_crash_simulated() on every write, so from that instant every write
// fake-succeeds and the on-disk state stays exactly as it was — and then invokes the restart callback, which
// reboots the HomeStore instance through the ordinary shutdown/boot path (the dying shutdown's writes, final
// CP included, are frozen out: that is precisely the crash semantics).  Each boot installs a fresh instance
// via HomeStore::with_crash_simulator, so the dying incarnation stays frozen through its whole teardown while
// the rebooted one starts writable, recovering as if from a real crash.
class CrashSimulator {
public:
    explicit CrashSimulator(std::function< void(void) > restart_cb = nullptr) : restart_cb_{std::move(restart_cb)} {}

private:
    template < typename... Args >
    friend bool crash_if_flip_fired(char const* flip_name, Args&&... args);
    friend bool is_crash_simulated();

    void crash_now() {
        crashed_.update([](auto* s) { *s = true; });
        if (restart_cb_) {
            // Restart on a separate thread: the crash point is reached from a reactor/io path that must keep
            // draining while the restart tears the instance down.
            std::thread t{[cb = std::move(restart_cb_)]() { cb(); }};
            t.detach();
        } else {
            raise(SIGKILL);
        }
    }

    sisl::Rcu::scoped_ptr< bool > crashed_; // value-initialized: not crashed until crash_now()
    std::function< void(void) > restart_cb_{nullptr};
};

/// The one-liner product crash points use: crash when `flip_name` fires.  Extra args forward to the flip's
/// condition match, so one flip name can target a subsystem/site (e.g. crash_before_sb_write conditioned on
/// the metablk name).  Returns true when the crash was taken so the caller unwinds immediately.
template < typename... Args >
inline bool crash_if_flip_fired(char const* flip_name, Args&&... args) {
    if (flip::Flip::instance().test_flip(flip_name, std::forward< Args >(args)...)) {
        crash_simulator().crash_now();
        return true;
    }
    return false;
}

/// True from the crash instant until the dying incarnation is gone.  RCU read — no atomic cost; this is the
/// PhysicalDev write gate's per-write check.
inline bool is_crash_simulated() {
    return Managers::has_crash_simulator() && *crash_simulator().crashed_.access().get();
}
#else
// Crash simulation does not exist in non-flip builds: both queries fold to false and the guarded branches
// compile out.
template < typename... Args >
inline bool crash_if_flip_fired(char const*, Args&&...) {
    return false;
}
inline bool is_crash_simulated() {
    return false;
}
#endif
} // namespace homestore
