#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>

#include <gtest/gtest.h>

#include "sharded_rate_limiter.h"

TEST(ShardedRateLimiter, DifferentKeysHaveIndependentBudgets) {
  ratelimiter::ShardedRateLimiter limiter(/*capacity=*/2,
                                           /*refill_rate_per_sec=*/0,
                                           /*num_shards=*/4);

  EXPECT_TRUE(limiter.tryAcquire("alice"));
  EXPECT_TRUE(limiter.tryAcquire("alice"));
  EXPECT_FALSE(limiter.tryAcquire("alice"));

  EXPECT_TRUE(limiter.tryAcquire("bob"));
  EXPECT_TRUE(limiter.tryAcquire("bob"));
  EXPECT_FALSE(limiter.tryAcquire("bob"));
}

TEST(ShardedRateLimiter, ManyKeysConcurrentlyEachRespectCapacity) {
  const double capacity = 10;
  ratelimiter::ShardedRateLimiter limiter(capacity, /*refill_rate_per_sec=*/0,
                                           /*num_shards=*/16);

  const int num_keys = 20;
  const int threads_per_key = 4;
  const int attempts_per_thread = 10;

  std::vector<std::thread> threads;
  std::vector<std::atomic<int>> success_counts(num_keys);
  for (auto& c : success_counts) {
    c.store(0);
  }

  for (int k = 0; k < num_keys; ++k) {
    std::string key = "user_" + std::to_string(k);
    for (int t = 0; t < threads_per_key; ++t) {
      threads.emplace_back([&limiter, key, &success_counts, k, attempts_per_thread]() {
        for (int i = 0; i < attempts_per_thread; ++i) {
          if (limiter.tryAcquire(key)) {
            success_counts[k].fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
  }

  for (auto& th : threads) {
    th.join();
  }

  for (int k = 0; k < num_keys; ++k) {
    EXPECT_EQ(success_counts[k].load(), static_cast<int>(capacity))
        << "key user_" << k << " got the wrong number of grants";
  }
}
