#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include "leader_elector.h"
#include "redis_backend.h"

namespace {

template <typename T>
T envOr(const char* name, T fallback);

template <>
std::string envOr<std::string>(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}

template <>
long envOr<long>(const char* name, long fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::strtol(value, nullptr, 10) : fallback;
}

template <>
double envOr<double>(const char* name, double fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::strtod(value, nullptr) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  // CLI values override environment variables.
  const std::string node_id =
      argc > 1 ? argv[1] : envOr<std::string>("NODE_ID", "node_default");
  const std::string redis_uri =
      argc > 2 ? argv[2]
               : envOr<std::string>("REDIS_URL", "tcp://127.0.0.1:6379");
  const long capacity =
      argc > 3 ? std::strtol(argv[3], nullptr, 10)
               : envOr<long>("RATE_LIMIT_CAPACITY", 10);
  const double refill_rate =
      argc > 4 ? std::strtod(argv[4], nullptr)
               : envOr<double>("RATE_LIMIT_REFILL_RATE", 2.0);
  const int ttl_seconds =
      argc > 5 ? std::atoi(argv[5])
               : static_cast<int>(envOr<long>("LEADER_TTL_SECONDS", 10));
  const int interval_ms =
      argc > 6 ? std::atoi(argv[6])
               : static_cast<int>(envOr<long>("SERVER_INTERVAL_MS", 500));

  std::cout << "[" << node_id << "] starting distributed rate limiter\n"
            << "  Redis: " << redis_uri << "\n"
            << "  capacity: " << capacity << "\n"
            << "  refill rate: " << refill_rate << " tokens/sec\n"
            << "  leader TTL: " << ttl_seconds << "s\n";

  RedisBackend backend(redis_uri);
  LeaderElector elector(redis_uri, "ratelimiter:leader_lock", node_id,
                        ttl_seconds);

  long request_count = 0;
  while (true) {
    if (!elector.isLeader()) {
      elector.tryAcquireLeadership();
    } else {
      elector.renewLeadership();
    }

    const bool allowed =
        backend.tryAcquire("client_demo", capacity, refill_rate);
    ++request_count;

    std::cout << "[" << node_id << "] request #" << request_count
              << " -> " << (allowed ? "ALLOWED" : "DENIED")
              << " | leader=" << (elector.isLeader() ? "yes" : "no")
              << '\n';

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}
