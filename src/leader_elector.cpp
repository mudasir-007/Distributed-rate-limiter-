#include "leader_elector.h"

#include <sw/redis++/redis++.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {

/**
 * @brief Generates a unique-enough token to identify one leadership
 * term. Combines the node's own ID with a random suffix so that even
 * if the same node acquires leadership twice in a row, each term has
 * a distinguishable token.
 */
std::string generateLeaseToken(const std::string& node_id) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<unsigned long long> dist;
  return node_id + ":" + std::to_string(dist(gen));
}

std::string loadScriptFromFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error(
        "LeaderElector: could not open " + path +
        " (check that the program is run from the project root)");
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

LeaderElector::LeaderElector(const std::string& connection_uri,
                              const std::string& election_key,
                              const std::string& node_id, int ttl_seconds)
    : redis_(std::make_unique<sw::redis::Redis>(connection_uri)),
      election_key_(election_key),
      node_id_(node_id),
      ttl_seconds_(ttl_seconds),
      is_leader_(false) {
  if (node_id_.empty()) {
    throw std::invalid_argument("LeaderElector: node_id must not be empty");
  }
  if (ttl_seconds_ <= 0) {
    throw std::invalid_argument("LeaderElector: ttl_seconds must be > 0");
  }

  const char* script_dir = std::getenv("RATE_LIMITER_SCRIPT_DIR");
  const std::string directory =
      (script_dir && *script_dir) ? script_dir : "scripts";
  std::string script =
      loadScriptFromFile(directory + "/leader_renew.lua");
  renew_script_sha_ = redis_->script_load(script);
}

LeaderElector::~LeaderElector() = default;

bool LeaderElector::tryAcquireLeadership() {
  std::string candidate_token = generateLeaseToken(node_id_);

  try {
    auto opts = sw::redis::UpdateType::NOT_EXIST;
    bool acquired = redis_->set(election_key_, candidate_token,
                                 std::chrono::seconds(ttl_seconds_), opts);

    if (acquired) {
      lease_token_ = candidate_token;
      is_leader_.store(true, std::memory_order_release);
    }
    return acquired;
  } catch (const sw::redis::Error& e) {
    std::cerr << "LeaderElector::tryAcquireLeadership failed: " << e.what()
              << std::endl;
    return false;
  }
}

bool LeaderElector::renewLeadership() {
  if (!is_leader_.load(std::memory_order_acquire)) {
    return false;
  }

  try {
    long long renewed = redis_->evalsha<long long>(
        renew_script_sha_, {election_key_},
        {lease_token_, std::to_string(ttl_seconds_)});

    bool success = (renewed == 1);
    is_leader_.store(success, std::memory_order_release);
    return success;
  } catch (const sw::redis::Error& e) {
    std::cerr << "LeaderElector::renewLeadership failed: " << e.what()
              << std::endl;
    is_leader_.store(false, std::memory_order_release);
    return false;
  }
}

bool LeaderElector::isLeader() const {
  return is_leader_.load(std::memory_order_acquire);
}
