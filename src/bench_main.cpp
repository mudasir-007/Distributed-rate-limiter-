#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sharded_rate_limiter.h"
#include "thread_safe_token_bucket.h"

using Clock = std::chrono::steady_clock;
using Micros = std::chrono::microseconds;

struct Metrics {
  std::string name;
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
  const double frac = index - static_cast<double>(lo);
  return values[lo] + (values[hi] - values[lo]) * frac;
}

static void printMetrics(const Metrics& m) {
  std::cout << "\n[" << m.name << "]\n"
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

static void writeCsv(const std::string& path, const std::vector<Metrics>& rows) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("Could not open CSV output: " + path);
  out << "scenario,requests,granted,denied,elapsed_ms,throughput_req_s,"
         "avg_us,p50_us,p95_us,p99_us,min_us,max_us\n";
  out << std::fixed << std::setprecision(4);
  for (const auto& m : rows) {
    out << '"' << m.name << '"' << ',' << m.requests << ',' << m.granted << ','
        << m.denied << ',' << m.elapsed_ms << ',' << m.throughput << ','
        << m.avg_us << ',' << m.p50_us << ',' << m.p95_us << ',' << m.p99_us
        << ',' << m.min_us << ',' << m.max_us << '\n';
  }
}

template <typename Operation>
Metrics runBenchmark(const std::string& name, int threads, int requestsPerThread,
                     Operation operation) {
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
        const bool allowed = operation(t, i);
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
  for (const auto& local : threadLatencies) {
    latencies.insert(latencies.end(), local.begin(), local.end());
  }

  Metrics m;
  m.name = name;
  m.requests = total;
  m.granted = std::accumulate(grants.begin(), grants.end(), std::size_t{0});
  m.denied = m.requests - m.granted;
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
  printMetrics(m);
  return m;
}

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 16;
  const int requestsPerThread = argc > 2 ? std::atoi(argv[2]) : 10000;
  const int keys = argc > 3 ? std::atoi(argv[3]) : 1000;
  const std::string csvPath = argc > 4 ? argv[4] : "";

  if (threads <= 0 || requestsPerThread <= 0 || keys <= 0) {
    std::cerr << "Usage: bench [threads] [requests_per_thread] [keys] [csv_path]\n";
    return 2;
  }

  std::cout << "Distributed Rate Limiter - In-Memory Benchmark\n"
            << "threads=" << threads
            << ", requests/thread=" << requestsPerThread
            << ", keys=" << keys << "\n";

  std::vector<Metrics> results;

  ratelimiter::ThreadSafeTokenBucket globalBucket(1e9, 1e9);
  results.push_back(runBenchmark(
      "single thread-safe bucket", threads, requestsPerThread,
      [&](int, int) { return globalBucket.tryAcquire(1.0); }));

  double baseline = results.back().elapsed_ms;
  for (std::size_t shards : {4u, 16u, 64u}) {
    ratelimiter::ShardedRateLimiter limiter(1e9, 1e9, shards);
    auto result = runBenchmark(
        "sharded limiter (" + std::to_string(shards) + " shards)", threads,
        requestsPerThread, [&](int t, int i) {
          const std::string key =
              "user_" + std::to_string((t * requestsPerThread + i) % keys);
          return limiter.tryAcquire(key);
        });
    std::cout << "  speedup vs single bucket: " << std::fixed
              << std::setprecision(2) << baseline / result.elapsed_ms << "x\n";
    results.push_back(result);
  }

  if (!csvPath.empty()) {
    writeCsv(csvPath, results);
    std::cout << "\nCSV written to: " << csvPath << '\n';
  }

  std::cout << "\nNote: benchmark numbers depend on CPU, OS, compiler, build mode,\n"
               "scheduler, and background load. Compare runs on the same machine\n"
               "and configuration; do not treat one run as a universal SLA.\n";
  return 0;
}
