#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sw/redis++/redis++.h>

#include "leader_elector.h"
#include "redis_backend.h"
#include "sharded_rate_limiter.h"
#include "thread_safe_token_bucket.h"

namespace {

struct Config {
  std::string redis_url = "tcp://127.0.0.1:6379";
  int threads = 32;
  int attempts = 50;
  int capacity = 100;
  int keys = 16;
};

std::string envString(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}

int envInt(const char* name, int fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::atoi(value) : fallback;
}

void banner(const std::string& title) {
  std::cout << "\n=== " << title << " ===\n";
}

bool localSingleBucket(const Config& c) {
  banner("1. ThreadSafeTokenBucket: single-key contention");
  ratelimiter::ThreadSafeTokenBucket bucket(c.capacity, 0.0);
  std::atomic<int> grants{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < c.threads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < c.attempts; ++i) {
        if (bucket.tryAcquire()) {
          grants.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();

  const int actual = grants.load();
  std::cout << "threads=" << c.threads << ", attempts/thread=" << c.attempts
            << ", capacity=" << c.capacity << "\n"
            << "grants=" << actual << " -> "
            << (actual == c.capacity ? "PASS" : "FAIL") << '\n';
  return actual == c.capacity;
}

bool shardedMultiKey(const Config& c) {
  banner("2. ShardedRateLimiter: concurrent independent keys");
  const int per_key_capacity = 10;
  ratelimiter::ShardedRateLimiter limiter(per_key_capacity, 0.0, 8);

  std::vector<std::atomic<int>> grants(c.keys);
  for (auto& value : grants) value.store(0);

  std::vector<std::thread> threads;
  for (int key_id = 0; key_id < c.keys; ++key_id) {
    const std::string key = "demo:user:" + std::to_string(key_id);
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&limiter, &grants, key, key_id]() {
        for (int i = 0; i < 20; ++i) {
          if (limiter.tryAcquire(key)) {
            grants[key_id].fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
  }
  for (auto& thread : threads) thread.join();

  bool pass = true;
  for (int key_id = 0; key_id < c.keys; ++key_id) {
    const int actual = grants[key_id].load();
    if (actual != per_key_capacity) pass = false;
  }

  std::cout << "keys=" << c.keys << ", 4 threads/key, capacity/key="
            << per_key_capacity << "\n";
  std::cout << "every key granted exactly its own capacity -> "
            << (pass ? "PASS" : "FAIL") << '\n';
  return pass;
}

bool redisSingleKey(const Config& c) {
  banner("3. RedisBackend: cross-thread atomicity");
  sw::redis::Redis redis(c.redis_url);
  const std::string key = "demo:redis:single:" +
                          std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count());
  redis.del(key);

  RedisBackend backend(c.redis_url);
  std::atomic<int> grants{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < c.threads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < c.attempts; ++i) {
        if (backend.tryAcquire(key, c.capacity, 0.0)) {
          grants.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();

  const int actual = grants.load();
  redis.del(key);
  std::cout << "threads=" << c.threads << ", total attempts="
            << c.threads * c.attempts << ", capacity=" << c.capacity << "\n"
            << "grants=" << actual << " -> "
            << (actual == c.capacity ? "PASS" : "FAIL") << '\n';
  return actual == c.capacity;
}

bool redisMultiKey(const Config& c) {
  banner("4. RedisBackend: concurrent multi-key isolation");
  sw::redis::Redis redis(c.redis_url);
  RedisBackend backend(c.redis_url);

  const int per_key_capacity = 10;
  std::vector<std::string> keys;
  std::vector<std::atomic<int>> grants(c.keys);
  for (auto& value : grants) value.store(0);

  for (int key_id = 0; key_id < c.keys; ++key_id) {
    keys.push_back("demo:redis:multi:" + std::to_string(key_id) + ":" +
                   std::to_string(std::chrono::steady_clock::now()
                                      .time_since_epoch()
                                      .count()));
    redis.del(keys.back());
  }

  std::vector<std::thread> threads;
  for (int key_id = 0; key_id < c.keys; ++key_id) {
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, key_id]() {
        for (int i = 0; i < 20; ++i) {
          if (backend.tryAcquire(keys[key_id], per_key_capacity, 0.0)) {
            grants[key_id].fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
  }
  for (auto& thread : threads) thread.join();

  bool pass = true;
  for (int key_id = 0; key_id < c.keys; ++key_id) {
    if (grants[key_id].load() != per_key_capacity) pass = false;
    redis.del(keys[key_id]);
  }

  std::cout << "keys=" << c.keys << ", 4 threads/key, capacity/key="
            << per_key_capacity << "\n"
            << "every Redis key granted exactly its own capacity -> "
            << (pass ? "PASS" : "FAIL") << '\n';
  return pass;
}

bool leaderElection(const Config& c) {
  banner("5. LeaderElector: concurrent acquisition + fencing");
  sw::redis::Redis redis(c.redis_url);
  const std::string key = "demo:leader:" +
                          std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count());
  redis.del(key);

  const int contenders = 16;
  std::vector<std::unique_ptr<LeaderElector>> electors;
  for (int i = 0; i < contenders; ++i) {
    electors.push_back(std::make_unique<LeaderElector>(
        c.redis_url, key, "demo-node-" + std::to_string(i), 2));
  }

  std::atomic<int> acquired{0};
  std::atomic<int> winner{-1};
  std::vector<std::thread> threads;
  for (int i = 0; i < contenders; ++i) {
    threads.emplace_back([&acquired, &winner, elector_ptr = electors[i].get(), i]() {
      if (elector_ptr->tryAcquireLeadership()) {
        winner.store(i, std::memory_order_release);
        acquired.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  bool pass = acquired.load() == 1;
  std::cout << contenders << " concurrent contenders -> "
            << acquired.load() << " acquired the lease -> "
            << (pass ? "PASS" : "FAIL") << '\n';

  const int winner_index = winner.load(std::memory_order_acquire);
  std::this_thread::sleep_for(std::chrono::seconds(3));

  LeaderElector fresh(c.redis_url, key, "fresh-node", 5);
  const bool fresh_acquired = fresh.tryAcquireLeadership();
  const bool stale_blocked =
      winner_index >= 0 && !electors[winner_index]->renewLeadership();

  std::cout << "after TTL: fresh node acquired=" << std::boolalpha
            << fresh_acquired << ", stale renewal blocked=" << stale_blocked
            << " -> "
            << (fresh_acquired && stale_blocked ? "PASS" : "FAIL") << '\n';

  redis.del(key);
  return pass && fresh_acquired && stale_blocked;
}

}  // namespace

int main() {
  Config config;
  config.redis_url = envString("REDIS_URL", config.redis_url);
  config.threads = envInt("DEMO_THREADS", config.threads);
  config.attempts = envInt("DEMO_ATTEMPTS", config.attempts);
  config.capacity = envInt("DEMO_CAPACITY", config.capacity);
  config.keys = envInt("DEMO_KEYS", config.keys);

  std::cout << "Distributed Rate Limiter Concurrency Demo\n"
            << "Redis: " << config.redis_url << '\n';

  try {
    const bool a = localSingleBucket(config);
    const bool b = shardedMultiKey(config);
    const bool c = redisSingleKey(config);
    const bool d = redisMultiKey(config);
    const bool e = leaderElection(config);

    std::cout << "\n========================================\n"
              << "OVERALL RESULT: "
              << ((a && b && c && d && e) ? "PASS" : "FAIL") << '\n'
              << "========================================\n";
    return (a && b && c && d && e) ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "\nDEMO ERROR: " << e.what() << '\n';
    std::cerr << "Make sure Redis is running and REDIS_URL is correct.\n";
    return 2;
  }
}
