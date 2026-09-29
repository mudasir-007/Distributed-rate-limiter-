#include "thread_safe_token_bucket.h"

namespace ratelimiter{

    ThreadSafeTokenBucket::ThreadSafeTokenBucket(double capacity,
                                                  double refill_rate_per_sec)
        : bucket_(capacity, refill_rate_per_sec) {}
    
    bool ThreadSafeTokenBucket::tryAcquire(double tokens){
        std::lock_guard<std::mutex> lock(mutex_);
        return bucket_.tryAcquire(tokens);
    }

    double ThreadSafeTokenBucket::peekTokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bucket_.peekTokens();
    }

} // namespace ratelimiter
