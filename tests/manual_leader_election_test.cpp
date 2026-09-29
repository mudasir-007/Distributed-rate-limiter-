#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

#include "leader_elector.h"

namespace {

std::string currentTimeString() {
  auto now = std::chrono::system_clock::now();
  std::time_t now_c = std::chrono::system_clock::to_time_t(now);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&now_c));
  return std::string(buf);
}

void runNode(const std::string& node_id, int duration_seconds) {
  LeaderElector elector("tcp://127.0.0.1:6379", "ratelimiter:leader_lock",
                         node_id, /*ttl_seconds=*/3);

  auto start = std::chrono::steady_clock::now();
  auto elapsed = [&]() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };

  while (elapsed() < duration_seconds) {
    if (!elector.isLeader()) {
      if (elector.tryAcquireLeadership()) {
        std::cout << "[" << node_id << "] " << currentTimeString()
                  << " (t=" << elapsed() << "s): ACQUIRED leadership\n";
      }
    } else {
      if (elector.renewLeadership()) {
        std::cout << "[" << node_id << "] " << currentTimeString()
                  << " (t=" << elapsed() << "s): renewed (still leader)\n";
      } else {
        std::cout << "[" << node_id << "] " << currentTimeString()
                  << " (t=" << elapsed() << "s): LOST leadership\n";
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string node_id = argc > 1 ? argv[1] : "node_unknown";
  int duration_seconds = argc > 2 ? std::atoi(argv[2]) : 30;

  std::cout << "Starting node '" << node_id << "' for " << duration_seconds
            << " seconds...\n";
  runNode(node_id, duration_seconds);

  return 0;
}