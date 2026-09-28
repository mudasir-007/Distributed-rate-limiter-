#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

namespace sw {
namespace redis {
class Redis;
}
}

/**
 * @brief Distributed leader election using Redis as the coordination
 * point.
 *
 * Multiple processes (potentially on different machines) can each
 * construct a LeaderElector pointed at the same Redis instance and the
 * same election key. At any moment, at most one of them holds the
 * lease and is considered "the leader." Leadership is implemented as a
 * Redis key with a TTL: holding the key means being leader, and the
 * lease must be renewed before it expires or another process can claim
 * it.
 *
 * THE FENCING PROBLEM — why this isn't just "SET NX EX" and done:
 * A naive implementation has a race: if the current leader is paused
 * (GC pause, frozen VM, slow disk) for longer than the TTL, Redis
 * expires its key and a second process becomes leader. If the first
 * process later wakes up and blindly renews its lease without
 * checking whether it's still actually holding it, it can clobber the
 * second process's legitimate leadership. This implementation prevents
 * that by giving each leadership term a unique token, and only
 * allowing a renewal if the caller's token still matches what's stored
 * in Redis (done atomically via a Lua script, see
 * scripts/leader_renew.lua).
 */
class LeaderElector {
public:
    /**
     * @brief Connects to Redis and prepares for leader election.
     * @param connection_uri Redis connection string, e.g. "tcp://127.0.0.1:6379".
     * @param election_key The Redis key used to represent the leadership lease.
     * @param node_id A unique identifier for this process/node (e.g. hostname + PID).
     * @param ttl_seconds How long a lease lasts before it must be renewed.
     */
    explicit LeaderElector(const std::string& connection_uri,
                  const std::string& election_key,
                  const std::string& node_id,
                  int ttl_seconds);
    
    ~LeaderElector();

    LeaderElector(const LeaderElector&) = delete;
    LeaderElector& operator=(const LeaderElector&) = delete;

    /**
     * @brief Attempts to become leader if no one currently holds the lease.
     * @return true if this call successfully acquired leadership.
     */
    bool tryAcquireLeadership();

    /**
     * @brief Renews this node's lease, but only if it still actually
     * holds it (fencing-safe — see class-level docs).
     * @return true if the renewal succeeded; false means leadership
     *         was lost (e.g. lease expired and someone else took over).
     */
    bool renewLeadership();

    /**
     * @brief Returns whether this node currently believes it is leader,
     * based on the last successful acquire/renew call. This is a local
     * cached flag, not a fresh check against Redis.
     */
    bool isLeader() const;

private:
    std::unique_ptr<sw::redis::Redis> redis_;
    std::string election_key_;
    std::string node_id_;
    std::string lease_token_;
    int ttl_seconds_;
    std::atomic<bool> is_leader_;
    std::string renew_script_sha_;
    
};