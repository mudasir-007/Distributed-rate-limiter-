#include "sharded_rate_limiter.h"

#include <functional>
#include <stdexcept>

namespace ratelimiter {

ShardedRateLimiter::ShardedRateLimiter(double capacity,
                                       double refill_rate_per_sec,
                                       size_t num_shards)
    : capacity_(capacity),
      refill_rate_per_sec_(refill_rate_per_sec) {
  if (num_shards == 0) {
    throw std::invalid_argument("ShardedRateLimiter: num_shards must be > 0");
  }

  shards_.reserve(num_shards);
  for (size_t i = 0; i < num_shards; ++i) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

size_t ShardedRateLimiter::shardIndexFor(const std::string& key) const {
  return std::hash<std::string>{}(key) % shards_.size();
}

bool ShardedRateLimiter::tryAcquire(const std::string& key, double tokens) {
  Shard& shard = *shards_[shardIndexFor(key)];

  ThreadSafeTokenBucket* bucket_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.buckets.find(key);
    if (it == shard.buckets.end()) {
      auto bucket = std::make_unique<ThreadSafeTokenBucket>(
          capacity_, refill_rate_per_sec_);
      it = shard.buckets.emplace(key, std::move(bucket)).first;
    }
    bucket_ptr = it->second.get();
  }

  // The bucket is never erased, so the pointer remains valid after the
  // shard-map lock is released. The bucket has its own mutex, allowing
  // different keys in the same shard to progress independently.
  return bucket_ptr->tryAcquire(tokens);
}

}  // namespace ratelimiter
