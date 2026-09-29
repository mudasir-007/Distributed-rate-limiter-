#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static void simulateExpensiveWork(std::chrono::microseconds delay) {
  const auto start = Clock::now();
  while (Clock::now() - start < delay) {
  }
}

static double runSingle(int threads, int requests, std::chrono::microseconds delay) {
  std::mutex mutex;
  std::vector<std::thread> workers;
  const auto start = Clock::now();

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&]() {
      for (int i = 0; i < requests; ++i) {
        std::lock_guard<std::mutex> lock(mutex);
        simulateExpensiveWork(delay);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static double runSharded(int threads, int requests, int keys, std::size_t shards,
                         std::chrono::microseconds delay) {
  std::vector<std::mutex> mutexes(shards);
  std::vector<std::thread> workers;
  const auto start = Clock::now();

  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      for (int i = 0; i < requests; ++i) {
        const int key = (t * requests + i) % keys;
        std::lock_guard<std::mutex> lock(mutexes[static_cast<std::size_t>(key) % shards]);
        simulateExpensiveWork(delay);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 16;
  const int requests = argc > 2 ? std::atoi(argv[2]) : 200;
  const int keys = argc > 3 ? std::atoi(argv[3]) : 1000;
  const int delayUs = argc > 4 ? std::atoi(argv[4]) : 500;

  if (threads <= 0 || requests <= 0 || keys <= 0 || delayUs < 0) {
    std::cerr << "Usage: bench_contention [threads] [requests/thread] "
                 "[keys] [critical_section_us]\n";
    return 2;
  }

  const auto delay = std::chrono::microseconds(delayUs);
  const std::size_t total =
      static_cast<std::size_t>(threads) * requests;

  std::cout << "Distributed Rate Limiter - Contention Benchmark\n"
            << "threads=" << threads
            << ", requests/thread=" << requests
            << ", keys=" << keys
            << ", critical-section=" << delayUs << " us\n\n";

  const double baseline = runSingle(threads, requests, delay);
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "single global lock | " << baseline << " ms | "
            << total / (baseline / 1000.0) << " req/s\n";

  for (std::size_t shards : {1u, 4u, 16u, 64u}) {
    const double elapsed = runSharded(threads, requests, keys, shards, delay);
    std::cout << std::setw(2) << shards << " shards          | "
              << elapsed << " ms | "
              << total / (elapsed / 1000.0) << " req/s | speedup "
              << baseline / elapsed << "x\n";
  }

  std::cout << "\nThis benchmark intentionally holds the lock during simulated work.\n"
               "It demonstrates contention behavior, not application latency.\n";
  return 0;
}
