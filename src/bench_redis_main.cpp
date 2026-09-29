#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sw/redis++/redis++.h>

#include "redis_backend.h"

using Clock = std::chrono::steady_clock;

struct Metrics {
  std::size_t requests{};
  std::size_t granted{};
  std::size_t denied{};
  double elapsed_ms{};
  double throughput{};
  double avg_us{};
  double p50_us{};
  double p95_us{};
  double p99_us{};
  double min_us{};
  double max_us{};
};

static double percentile(std::vector<double>& values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double index = p * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(index);
  const auto hi = std::min(lo + 1, values.size() - 1);
  return values[lo] + (values[hi] - values[lo]) * (index - lo);
}

static void print(const Metrics& m) {
  std::cout << "\nRedis backend benchmark\n"
            << "  requests       : " << m.requests << '\n'
            << "  granted        : " << m.granted << '\n'
            << "  denied         : " << m.denied << '\n'
            << "  elapsed        : " << std::fixed << std::setprecision(3)
            << m.elapsed_ms << " ms\n"
            << "  throughput     : " << std::setprecision(2) << m.throughput
            << " req/s\n"
            << "  latency avg    : " << m.avg_us << " us\n"
            << "  latency p50    : " << m.p50_us << " us\n"
            << "  latency p95    : " << m.p95_us << " us\n"
            << "  latency p99    : " << m.p99_us << " us\n"
            << "  latency min    : " << m.min_us << " us\n"
            << "  latency max    : " << m.max_us << " us\n";
}

int main(int argc, char** argv) {
  const std::string redisUrl =
      argc > 1 ? argv[1]
               : (std::getenv("REDIS_URL") ? std::getenv("REDIS_URL")
                                           : "tcp://127.0.0.1:6379");
  const int threads = argc > 2 ? std::atoi(argv[2]) : 8;
  const int requestsPerThread = argc > 3 ? std::atoi(argv[3]) : 1000;
  const long capacity = argc > 4 ? std::atol(argv[4]) : 1000000000L;

  if (threads <= 0 || requestsPerThread <= 0 || capacity <= 0) {
    std::cerr << "Usage: bench_redis [redis_url] [threads] "
                 "[requests_per_thread] [capacity]\n";
    return 2;
  }

  sw::redis::Redis redis(redisUrl);
  const std::string key =
      "bench:redis:" +
      std::to_string(Clock::now().time_since_epoch().count());
  redis.del(key);

  RedisBackend backend(redisUrl);
  const std::size_t total =
      static_cast<std::size_t>(threads) * requestsPerThread;
  std::vector<std::vector<double>> threadLatencies(threads);
  std::vector<std::size_t> grants(threads, 0);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  const auto start = Clock::now();
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      auto& latencies = threadLatencies[t];
      latencies.reserve(requestsPerThread);
      for (int i = 0; i < requestsPerThread; ++i) {
        const auto requestStart = Clock::now();
        const bool allowed = backend.tryAcquire(key, capacity, 0.0);
        const auto requestEnd = Clock::now();
        latencies.push_back(
            std::chrono::duration<double, std::micro>(requestEnd - requestStart)
                .count());
        if (allowed) ++grants[t];
      }
    });
  }
  for (auto& worker : workers) worker.join();
  const auto end = Clock::now();

  std::vector<double> latencies;
  latencies.reserve(total);
  for (const auto& local : threadLatencies)
    latencies.insert(latencies.end(), local.begin(), local.end());

  Metrics m;
  m.requests = total;
  m.granted = std::accumulate(grants.begin(), grants.end(), std::size_t{0});
  m.denied = total - m.granted;
  m.elapsed_ms =
      std::chrono::duration<double, std::milli>(end - start).count();
  m.throughput = total / (m.elapsed_ms / 1000.0);
  m.avg_us =
      std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();
  m.p50_us = percentile(latencies, 0.50);
  m.p95_us = percentile(latencies, 0.95);
  m.p99_us = percentile(latencies, 0.99);
  auto [minIt, maxIt] = std::minmax_element(latencies.begin(), latencies.end());
  m.min_us = *minIt;
  m.max_us = *maxIt;

  print(m);
  redis.del(key);
  std::cout << "\nRedis URL: " << redisUrl << '\n';
  return 0;
}
