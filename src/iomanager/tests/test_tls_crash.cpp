// Root cause: folly::rcu_default_domain() + thread exit crashes
// Test with and without folly::Init
#include <cstdio>
#include <thread>
#include <folly/synchronization/Rcu.h>
#include <folly/init/Init.h>

int main(int argc, char* argv[]) {
    folly::Init folly_init(&argc, &argv, folly::InitOptions{}.useGFlags(false));

    fprintf(stderr, "=== Test: with folly::Init ===\n");
    {
        std::thread t([]() {
            { std::unique_lock<folly::rcu_domain> g{folly::rcu_default_domain()}; }
            fprintf(stderr, "RCU used on thread\n");
        });
        t.join();
        fprintf(stderr, "OK\n");
    }

    fprintf(stderr, "All done.\n");
    return 0;
}
