#pragma once

#include <chrono>

namespace ratelimiter {

// TokenBucket implements the classic token-bucket algorithm.
//
// Deliberately NOT thread-safe. The algorithm is built and unit-tested
// in isolation first; concurrency is layered on top separately in
// ThreadSafeTokenBucket. Keeping these separate means any bug found
// later is unambiguously either an algorithm bug or a concurrency bug,
// never both tangled together.
//
// Semantics:
//   - capacity_: max tokens the bucket can hold (defines burst size)
//   - refill_rate_: tokens added per second
//   - Tokens refill lazily on each call to tryAcquire(), based on elapsed
//     wall-clock time since the last refill. There is no background
//     thread ticking the bucket, which avoids a class of synchronization
//     bugs where a timer thread races with request-handling threads.
class TokenBucket {
 public:
  // capacity: maximum tokens the bucket can hold (burst allowance)
  // refill_rate_per_sec: tokens added per second (sustained throughput)
  TokenBucket(double capacity, double refill_rate_per_sec);

  // Attempts to consume `tokens` tokens (default 1). Returns true if
  // allowed, false if the bucket doesn't have enough tokens.
  // Side effect: refills the bucket based on elapsed time before checking.
  bool tryAcquire(double tokens = 1.0);

  // Returns the current token count without consuming any and without
  // refilling. Useful for tests and metrics, not for the hot path.
  double peekTokens() const { return tokens_; }

  double capacity() const { return capacity_; }
  double refillRate() const { return refill_rate_per_sec_; }

 private:
  // Adds tokens based on time elapsed since last_refill_, capped at
  // capacity_. Updates last_refill_ to now.
  void refill();

  double capacity_;
  double refill_rate_per_sec_;
  double tokens_;
  std::chrono::steady_clock::time_point last_refill_;
};

}  // namespace ratelimiter