#include "redis_backend.h"

#include <cassert>
#include <iostream>

// Manual integration test for RedisBackend.
//
// Unlike manual_stress_test.cpp (which tests purely in-memory state),
// this test requires a real, reachable Redis instance, since
// RedisBackend's whole purpose is coordinating state that lives outside
// the process. Run `docker exec redis-dev redis-cli ping` first to
// confirm Redis is up before running this.
//
// This deliberately uses a single thread: the goal here is to verify
// correctness of the refill/drain/refill cycle against a real backend,
// not to stress-test concurrency (that's covered by
// manual_multi_key_stress_test.cpp's design, and could be extended to
// hit RedisBackend directly in a future task if desired).

int main() {
    RedisBackend backend("tcp://127.0.0.1:6379");

    const std::string key = "ratelimit:manual_test";
    const long capacity = 5;
    const double refill_rate = 1.0;  // 1 token per second

    // Start from a clean slate: this test is not idempotent against
    // leftover state from a previous run, so we rely on the bucket's
    // 1-hour TTL eventually clearing it, OR the person running this
    // manually clearing it first with:
    //   docker exec redis-dev redis-cli DEL ratelimit:manual_test

    int successful_count = 0;
    for (int i = 0; i < capacity; ++i) {
        bool acquired = backend.tryAcquire(key, capacity, refill_rate);
        if (acquired) {
            ++successful_count;
        } else {
            std::cout << "Unexpected denial at request " << i
                      << " (capacity=" << capacity << ")" << std::endl;
        }
    }

    assert(successful_count == capacity);
    std::cout << "Drained bucket: " << successful_count << "/" << capacity
              << " requests succeeded (expected all to succeed)."
              << std::endl;

    bool should_be_denied = backend.tryAcquire(key, capacity, refill_rate);
    assert(should_be_denied == false);
    std::cout << "Bucket correctly denied request " << (capacity + 1)
              << " after exhausting capacity." << std::endl;

    std::cout << "All assertions passed." << std::endl;
    return 0;
}