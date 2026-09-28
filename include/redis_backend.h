#pragma once

#include <string>
#include <mutex>
#include <memory>

namespace sw
{
    namespace redis
    {
        class Redis;
    }
}

/**
 * @brief Distributed rate limiter backend using Redis as shared state.
 *
 * Unlike ShardedRateLimiter (which holds bucket state in local process
 * memory), RedisBackend stores token bucket state in Redis, making it
 * safe for multiple server instances to share rate limits for the same
 * client key. All read-check-decrement logic for a single call happens
 * atomically server-side via a Lua script, avoiding race conditions that
 * would occur if refill/check/decrement were sent as separate round trips.
 */
class RedisBackend
{
public:
    /**
     * @brief Connects to Redis and prepares the rate limiting script.
     * @param connection_uri Redis connection string, e.g. "tcp://127.0.0.1:6379".
     */
    explicit RedisBackend(const std::string &connection_uri);

    /**
     * @brief Closes the Redis connection. Defined in the .cpp file since
     * the real sw::redis::Redis type must be visible for unique_ptr's
     * destructor logic to be generated correctly.
     */
    ~RedisBackend();

    RedisBackend(const RedisBackend &) = delete;
    RedisBackend &operator=(const RedisBackend &) = delete;

    /**
     * @brief Attempts to consume one token for the given client key.
     *
     * Executes the rate limiting Lua script atomically on the Redis
     * server: refills tokens based on elapsed time since last access,
     * checks if at least one token is available, and decrements if so.
     *
     * @param key Unique identifier for the client/resource being limited.
     * @param capacity Maximum tokens the bucket can hold.
     * @param refill_rate Tokens added per second.
     * @return true if a token was successfully consumed, false if the
     *         bucket was empty (request should be rejected/throttled).
     */
    bool tryAcquire(const std::string &key, long capacity, double refill_rate);

private:
    /**
     * @brief Loads the Lua script into Redis via SCRIPT LOAD and caches
     * its SHA1 hash so future calls can use the cheaper EVALSHA instead
     * of resending the full script body every time.
     * @return The script's SHA1 hash.
     */
    std::string loadScriptSha();
    std::string loadScriptText() const;

    std::unique_ptr<sw::redis::Redis> redis_;
    std::string script_sha_;
    std::string script_text_;
    mutable std::mutex script_mutex_;
};
