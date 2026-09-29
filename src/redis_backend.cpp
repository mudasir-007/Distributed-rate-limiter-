#include "redis_backend.h"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

std::string scriptDirectory() {
    const char* value = std::getenv("RATE_LIMITER_SCRIPT_DIR");
    return value && *value ? value : "scripts";
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "RedisBackend: could not open " + path +
            ". Set RATE_LIMITER_SCRIPT_DIR or run from the project root.");
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

RedisBackend::RedisBackend(const std::string& connection_uri)
    : redis_(std::make_unique<sw::redis::Redis>(connection_uri)),
      script_text_(loadScriptText()) {
    script_sha_ = redis_->script_load(script_text_);
}

RedisBackend::~RedisBackend() = default;

std::string RedisBackend::loadScriptText() const {
    return readFile(scriptDirectory() + "/token_bucket.lua");
}

std::string RedisBackend::loadScriptSha() {
    script_text_ = loadScriptText();
    return redis_->script_load(script_text_);
}

bool RedisBackend::tryAcquire(const std::string& key, long capacity,
                              double refill_rate) {
    if (key.empty() || capacity <= 0 || !std::isfinite(refill_rate) ||
        refill_rate < 0.0) {
        return false;
    }

    try {
        long long allowed = redis_->evalsha<long long>(
            script_sha_,
            {key},
            {std::to_string(capacity), std::to_string(refill_rate)});
        return allowed == 1;
    } catch (const sw::redis::Error& e) {
        // Redis can lose loaded scripts after a restart/failover. Retry only
        // for the explicit NOSCRIPT case; retrying arbitrary connection errors
        // could duplicate a request whose first execution actually succeeded.
        const std::string message = e.what();
        if (message.find("NOSCRIPT") == std::string::npos) {
            std::cerr << "RedisBackend::tryAcquire failed, denying request: "
                      << message << '\n';
            return false;
        }

        try {
            std::lock_guard<std::mutex> lock(script_mutex_);
            // Another thread may already have restored the script. Reloading
            // is harmless and keeps recovery simple and deterministic.
            script_sha_ = loadScriptSha();
            long long allowed = redis_->evalsha<long long>(
                script_sha_,
                {key},
                {std::to_string(capacity), std::to_string(refill_rate)});
            return allowed == 1;
        } catch (const sw::redis::Error& retry_error) {
            std::cerr << "RedisBackend::tryAcquire NOSCRIPT recovery failed, "
                         "denying request: "
                      << retry_error.what() << '\n';
            return false;
        }
    }
}
