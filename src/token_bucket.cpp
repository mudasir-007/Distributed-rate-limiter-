#include "token_bucket.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ratelimiter {

TokenBucket::TokenBucket(double capacity, double refill_rate_per_sec)
    : capacity_(capacity),
      refill_rate_per_sec_(refill_rate_per_sec),
      tokens_(capacity),
      last_refill_(std::chrono::steady_clock::now()) {
  if (!std::isfinite(capacity_) || capacity_ <= 0.0) {
    throw std::invalid_argument("TokenBucket: capacity must be finite and > 0");
  }
  if (!std::isfinite(refill_rate_per_sec_) || refill_rate_per_sec_ < 0.0) {
    throw std::invalid_argument(
        "TokenBucket: refill_rate_per_sec must be finite and >= 0");
  }
}

void TokenBucket::refill() {
  const auto now = std::chrono::steady_clock::now();
  const double elapsed_seconds =
      std::chrono::duration<double>(now - last_refill_).count();

  if (elapsed_seconds <= 0.0) {
    return;
  }

  tokens_ = std::min(capacity_, tokens_ + elapsed_seconds * refill_rate_per_sec_);
  last_refill_ = now;
}

bool TokenBucket::tryAcquire(double tokens) {
  if (!std::isfinite(tokens) || tokens <= 0.0) {
    return false;
  }

  refill();

  if (tokens <= tokens_) {
    tokens_ -= tokens;
    return true;
  }
  return false;
}

}  // namespace ratelimiter
