#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "thread_safe_token_bucket.h"

namespace ratelimiter {

// ShardedRateLimiter maintains one TokenBucket per client key (e.g. user
// ID or API key), partitioned across N shards, each with its own mutex.
//
// A single global mutex protecting one shared map of buckets would
// serialize every request regardless of which client it belongs to —
// client A's request would block behind client B's lock even though
// they touch completely unrelated state. Sharding by hash(key) %
// num_shards means two requests for different clients usually land in
// different shards and never contend for the same lock.

class ShardedRateLimiter {
 public:
  ShardedRateLimiter(double capacity, double refill_rate_per_sec,
                     size_t num_shards = 16);
  // Looks up (or lazily creates) the bucket for `key` and attempts to
  // acquire `tokens` from it
  bool tryAcquire(const std::string&key, double tokens =1.0);

 private:
  struct Shard {
    std::mutex mutex;
    std::unordered_map<std::string,std::unique_ptr<ThreadSafeTokenBucket>> buckets;
  };

  size_t shardIndexFor(const std::string& key) const;

  double capacity_;
  double refill_rate_per_sec_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace ratelimiter
