#pragma once

#include <mutex>

#include "token_bucket.h"

namespace ratelimiter {

// Wraps TokenBucket with a std::mutex to make it safe for concurrent
// access from multiple threads within a single process.
//
// tryAcquire() is a compound operation: refill (read elapsed time,
// compute new token count, compare against capacity, write tokens_)
// followed by a check-and-decrement. Two threads racing through this
// sequence could both read the same pre-refill token count, both
// decide they are allowed to proceed, and both decrement — a lost
// update. A single mutex protects the entire critical section, not
// just one variable, which is what a sequence like this requires.
class ThreadSafeTokenBucket {
    public:
        ThreadSafeTokenBucket(double capacity, double refill_rate_per_sec);
        
        bool tryAcquire(double tokens = 1.0);

        double peekTokens() const;

        private:
            mutable std::mutex mutex_;
            TokenBucket bucket_;
};

}  // namespace ratelimiter