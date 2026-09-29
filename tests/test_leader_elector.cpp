#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include "leader_elector.h"

// These tests require a live Redis instance at tcp://127.0.0.1:6379.
// In CI, this is provided by a Redis service container.
// Locally, start Redis with: docker start redis-dev

static const std::string kRedisUri = "tcp://127.0.0.1:6379";
static const std::string kElectionKey = "test:leader_lock";

class LeaderElectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Clean up any leftover key from a previous test run so each test
    // starts with a guaranteed clean slate.
    sw::redis::Redis redis(kRedisUri);
    redis.del(kElectionKey);
  }

  void TearDown() override {
    sw::redis::Redis redis(kRedisUri);
    redis.del(kElectionKey);
  }
};

TEST_F(LeaderElectorTest, AcquiresLeadershipWhenKeyIsAbsent) {
  LeaderElector elector(kRedisUri, kElectionKey, "node_test", /*ttl=*/5);
  EXPECT_FALSE(elector.isLeader());
  EXPECT_TRUE(elector.tryAcquireLeadership());
  EXPECT_TRUE(elector.isLeader());
}

TEST_F(LeaderElectorTest, SecondNodeCannotAcquireWhileFirstHoldsLease) {
  LeaderElector first(kRedisUri, kElectionKey, "node_first", /*ttl=*/5);
  LeaderElector second(kRedisUri, kElectionKey, "node_second", /*ttl=*/5);

  EXPECT_TRUE(first.tryAcquireLeadership());
  EXPECT_FALSE(second.tryAcquireLeadership());
  EXPECT_TRUE(first.isLeader());
  EXPECT_FALSE(second.isLeader());
}

TEST_F(LeaderElectorTest, RenewalSucceedsWhileLeadershipIsHeld) {
  LeaderElector elector(kRedisUri, kElectionKey, "node_test", /*ttl=*/5);
  EXPECT_TRUE(elector.tryAcquireLeadership());
  EXPECT_TRUE(elector.renewLeadership());
  EXPECT_TRUE(elector.isLeader());
}

TEST_F(LeaderElectorTest, RenewalFailsAfterLeaseExpires) {
  LeaderElector elector(kRedisUri, kElectionKey, "node_test", /*ttl=*/1);
  EXPECT_TRUE(elector.tryAcquireLeadership());

  // Wait for the 1-second TTL to expire.
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Renewal should now fail: the key expired, so the token no longer
  // matches what's stored (nothing is stored at all).
  EXPECT_FALSE(elector.renewLeadership());
  EXPECT_FALSE(elector.isLeader());
}

TEST_F(LeaderElectorTest, FencingPreventsStaleLeaderFromRenewing) {
  LeaderElector stale(kRedisUri, kElectionKey, "node_stale", /*ttl=*/1);
  EXPECT_TRUE(stale.tryAcquireLeadership());

  // Lease expires.
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // New legitimate leader acquires.
  LeaderElector fresh(kRedisUri, kElectionKey, "node_fresh", /*ttl=*/5);
  EXPECT_TRUE(fresh.tryAcquireLeadership());

  // Stale node tries to renew — fencing should block it because its
  // token no longer matches what's stored in Redis.
  EXPECT_FALSE(stale.renewLeadership());
  EXPECT_TRUE(fresh.isLeader());
}