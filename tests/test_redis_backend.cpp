#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include "redis_backend.h"

static const std::string kRedisUri = "tcp://127.0.0.1:6379";

class RedisBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sw::redis::Redis redis(kRedisUri);
    key_ = "test:redis_backend:" +
           std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count());
    redis.del(key_);
  }

  void TearDown() override {
    sw::redis::Redis redis(kRedisUri);
    redis.del(key_);
  }

  std::string key_;
};

TEST_F(RedisBackendTest, AllowsRequestsWithinCapacity) {
  RedisBackend backend(kRedisUri);
  EXPECT_TRUE(backend.tryAcquire(key_, 5, 0));
}

TEST_F(RedisBackendTest, DeniesRequestsOnceBucketIsExhausted) {
  RedisBackend backend(kRedisUri);

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(backend.tryAcquire(key_, 5, 0));
  }

  EXPECT_FALSE(backend.tryAcquire(key_, 5, 0));
}

TEST_F(RedisBackendTest, RefillsTokensOverTime) {
  RedisBackend backend(kRedisUri);

  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(backend.tryAcquire(key_, 3, 10));
  }
  EXPECT_FALSE(backend.tryAcquire(key_, 3, 10));

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_TRUE(backend.tryAcquire(key_, 3, 10));
}
