#include "token_bucket.h"

#include <thread>
#include <stdexcept>

#include <gtest/gtest.h>

namespace ratelimiter {

TEST(TokenBucket, StartsFullAndAllowsBurstUpToCapacity) {
  TokenBucket bucket(/*capacity=*/5, /*refill_rate_per_sec=*/1);

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(bucket.tryAcquire()) << "request " << i << " should succeed";
  }
  EXPECT_FALSE(bucket.tryAcquire()) << "6th request should exceed capacity";
}

TEST(TokenBucket, RefillsOverTime) {
  TokenBucket bucket(/*capacity=*/2, /*refill_rate_per_sec=*/10);

  EXPECT_TRUE(bucket.tryAcquire(2));   // drain it
  EXPECT_FALSE(bucket.tryAcquire(1));  // empty

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  // ~10 tokens/sec * 0.15s ≈ 1.5 tokens refilled — enough for one more.
  EXPECT_TRUE(bucket.tryAcquire(1));
}

TEST(TokenBucket, NeverExceedsCapacityEvenAfterLongIdle) {
  TokenBucket bucket(/*capacity=*/3, /*refill_rate_per_sec=*/100);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Plenty of simulated refill time has passed; bucket should still cap
  // at capacity, not overflow.
  EXPECT_TRUE(bucket.tryAcquire(3));
  EXPECT_FALSE(bucket.tryAcquire(1));
}

TEST(TokenBucket, RejectsRequestLargerThanCapacity) {
  TokenBucket bucket(/*capacity=*/5, /*refill_rate_per_sec=*/1);
  EXPECT_FALSE(bucket.tryAcquire(10));
}

}  // namespace ratelimiter
