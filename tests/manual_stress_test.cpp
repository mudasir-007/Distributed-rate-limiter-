#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "thread_safe_token_bucket.h"

int main() {
  const double capacity = 100;
  ratelimiter::ThreadSafeTokenBucket bucket(capacity, /*refill_rate_per_sec=*/0.0);

  const int num_threads = 16;
  const int attempts_per_thread = 50;

  std::atomic<int> successful_count{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < attempts_per_thread; ++i) {
        if (bucket.tryAcquire(1)) {
          successful_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  std::cout << "successful_count = " << successful_count.load()
            << " (capacity = " << capacity << ")\n";

  if (successful_count.load() == static_cast<int>(capacity)) {
    std::cout << "PASS: exactly capacity tokens were granted under concurrent load.\n";
    return 0;
  } else {
    std::cout << "FAIL: expected exactly " << capacity << " grants.\n";
    return 1;
  }
}