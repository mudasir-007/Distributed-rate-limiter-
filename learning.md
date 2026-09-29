PAGE 1 — Problem, Goals, and Architecture
1.1 The Problem
Any API, service, or backend exposed to the internet needs protection against abuse. Without limits, a single misbehaving client — a buggy script, a scraper, a malicious actor, or even a legitimate client retrying too aggressively — can exhaust server resources and degrade service for everyone else.

A rate limiter answers one question, millions of times per second:

"Should this request from this client be allowed to proceed right now?"

The answer must be fast, correct under concurrency, and — when multiple server instances exist — consistent across all of them.

1.2 Why Naive Solutions Fail
Naive approach	Failure mode
Counter with no time window	Counts forever; a client that sends 100 requests in a year is eventually blocked.
Fixed window counter	Burst at window boundaries: 100 requests at 11:59:59 and 100 at 12:00:00 = 200 in two seconds.
Single mutex + single bucket	Every request for every client serializes on one lock. Throughput collapses under load.
In-memory state per server	Two servers behind a load balancer each grant the full limit → the client gets 2× the intended rate.
Redis INCR + EXPIRE as separate calls	Race condition: two servers both read count=99, both increment to 100, both allow.
This project solves each of these with a specific, defensible design decision.

1.3 Design Goals
Correct algorithm — token bucket, which allows controlled bursts while enforcing a sustained rate.

Thread-safe in-process operation — safe under heavy multi-threaded load.

Scalable in-process operation — sharding to reduce lock contention.

Distributed correctness — shared state across processes via Redis, with atomic operations.

Coordination safety — leader election with fencing to prevent split-brain.

Reproducibility — CMake + vcpkg + Docker, plus benchmarks anyone can run.

1.4 High-Level Architecture
text
                         ┌─────────────────────────┐
   Client requests ─────▶ │   Application Server    │
                         │                         │
                         │  ┌───────────────────┐  │
                         │  │ Rate Limiter API  │  │
                         │  └─────────┬─────────┘  │
                         │            │            │
                         │   ┌────────┴────────┐   │
                         │   ▼                 ▼   │
                         │ ┌──────────┐  ┌────────┐│
                         │ │In-Memory │  │Sharded ││
                         │ │ Bucket   │  │Limiter ││
                         │ └──────────┘  └────────┘│
                         │            │            │
                         │            ▼            │
                         │   ┌──────────────────┐  │
                         │   │  RedisBackend    │  │
                         │   └────────┬─────────┘  │
                         └────────────┼────────────┘
                                      │ EVALSHA
                                      ▼
                         ┌─────────────────────────┐
                         │        Redis            │
                         │  ┌───────────────────┐  │
                         │  │ token_bucket.lua  │  │  ← atomic
                         │  │ leader_renew.lua  │  │  ← fencing
                         │  └───────────────────┘  │
                         └─────────────────────────┘
                                      ▲
                         ┌────────────┴────────────┐
                         │  Other server instances │
                         │  (same Redis, same keys)│
                         └─────────────────────────┘
1.5 The Three Operating Modes
Mode 1 — In-memory (TokenBucket + ThreadSafeTokenBucket)
Single process, single client key or few keys. Lowest latency (~sub-microsecond). Cannot coordinate across processes.

Mode 2 — Sharded in-memory (ShardedRateLimiter)
Single process, many client keys. Keys are hashed across N shards, each with its own lock. Reduces contention dramatically.

Mode 3 — Redis-backed (RedisBackend)
Multiple processes/machines. State lives in Redis; atomicity guaranteed by Lua. Latency dominated by network round trip (~hundreds of microseconds to milliseconds).

PAGE 2 — The Core Algorithm
2.1 Token Bucket Semantics
Imagine a bucket with a maximum capacity of C tokens. Tokens drip in at a rate of R tokens per second. Each request consumes one token. If the bucket is empty, the request is rejected (or queued).

Two independent parameters:

capacity (C) — controls burst size. A client can spend up to C tokens instantly if it has been idle.

refill_rate (R) — controls sustained throughput. Over a long period, the client is limited to R requests per second.

Why this is better than a fixed counter: It naturally smooths bursts. A client that has been idle accumulates tokens up to C, then can burst. A client hammering continuously is throttled to R.

Example: C = 10, R = 2/sec.

Client sends 10 requests instantly → all 10 allowed (bucket empties).

Client sends an 11th request immediately → rejected.

Client waits 3 seconds → 6 tokens refilled (capped at 10) → next 6 requests allowed.

2.2 Lazy Refill — the Key Implementation Choice
There is no background thread replenishing tokens. Instead, tokens are computed on demand from elapsed wall-clock time:

cpp
void TokenBucket::refill() {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_refill_).count();
    if (elapsed <= 0.0) return;
    tokens_ = std::min(capacity_, tokens_ + elapsed * refill_rate_per_sec_);
    last_refill_ = now;
}
Why lazy refill is correct and better:

Alternative	Problem
Background timer thread	Extra thread per bucket, or one timer thread with a global lock — a synchronization bottleneck and a source of race bugs.
Refill on every read from a separate thread	Unnecessary wakeups; wastes CPU when idle.
Lazy refill	Zero cost when idle. Computation is O(1) per request. Matches wall-clock time exactly.
std::chrono::steady_clock vs system_clock: steady_clock is monotonic — it never jumps backwards when NTP adjusts the system clock. Using system_clock could produce negative elapsed time and either over-refill or stall the bucket.

2.3 The Acquire Operation
cpp
bool TokenBucket::tryAcquire(double tokens) {
    if (!std::isfinite(tokens) || tokens <= 0.0) return false;
    refill();
    if (tokens <= tokens_) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}
Note the API accepts a fractional tokens argument. This means a single call can consume more than one token — useful for endpoints with different costs (e.g., a search costs 1 token, an export costs 5 tokens).

2.4 Constructor Validation
cpp
TokenBucket::TokenBucket(double capacity, double refill_rate_per_sec)
    : capacity_(capacity), refill_rate_per_sec_(refill_rate_per_sec),
      tokens_(capacity), last_refill_(std::chrono::steady_clock::now())
{
    if (!std::isfinite(capacity_) || capacity_ <= 0.0)
        throw std::invalid_argument("capacity must be finite and > 0");
    if (!std::isfinite(refill_rate_per_sec_) || refill_rate_per_sec_ < 0.0)
        throw std::invalid_argument("refill_rate must be finite and >= 0");
}
Why validate in the constructor: Fail fast at configuration time, not at request time. A misconfigured limiter (e.g., NaN capacity from a bad config file) would otherwise silently misbehave in production. Note that refill_rate of 0 is legal — that creates a "no refill" bucket, useful for hard one-time quotas.

2.5 Thread-Safe Wrapper
TokenBucket is intentionally not thread-safe. Why? Because making it thread-safe would force every test of the algorithm to also reason about concurrency. By separating concerns:

TokenBucket tests → pure algorithm correctness

ThreadSafeTokenBucket tests → concurrency correctness

A bug in either layer is unambiguously attributable.

cpp
class ThreadSafeTokenBucket {
public:
    bool tryAcquire(double tokens = 1.0) {
        std::lock_guard<std::mutex> lock(mutex_);
        return bucket_.tryAcquire(tokens);   // refill + check + decrement
    }
    double peekTokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bucket_.peekTokens();
    }
private:
    mutable std::mutex mutex_;
    TokenBucket bucket_;
};
2.6 Why the Whole Operation Must Be Locked
A common mistake is to lock only the decrement:

cpp
// WRONG
double avail = bucket_.peekTokens();   // lock, read, unlock
if (avail >= 1.0) {
    bucket_.consume(1.0);              // lock, decrement, unlock
    return true;
}
Two threads can both read avail = 1.0, both pass the check, and both consume — the bucket goes to −1 and two requests are allowed when only one token existed. This is a classic TOCTOU (time-of-check to time-of-use) race and a lost update.

The correct design holds one lock across refill → compare → decrement. This is the single most important concurrency invariant in the entire project.

PAGE 3 — Sharding, Redis, and Atomicity
3.1 The Contention Problem
Suppose your service has 50,000 active clients. A single ThreadSafeTokenBucket per client is correct, but you need a map from key → bucket. If that map has one lock, then every request for every client serializes — client A's request blocks on client B's lock, even though they share no state.

The benchmark in the repo demonstrates this: a global lock yields roughly 2,000 req/s, while 64 shards yield roughly 23,000 req/s in the contention benchmark — over 10× improvement.

3.2 Sharded Design
Keys are partitioned across N shards:

text
shard_index = std::hash<std::string>{}(key) % num_shards
Each shard holds:

cpp
struct Shard {
    std::mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<ThreadSafeTokenBucket>> buckets;
};
Two-level locking:

Shard map lock — held only long enough to find-or-create the bucket pointer.

Per-bucket lock — held during tryAcquire.

cpp
bool ShardedRateLimiter::tryAcquire(const std::string& key, double tokens) {
    Shard& shard = *shards_[shardIndexFor(key)];
    ThreadSafeTokenBucket* bucket_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.buckets.find(key);
        if (it == shard.buckets.end()) {
            auto bucket = std::make_unique<ThreadSafeTokenBucket>(capacity_, refill_rate_);
            it = shard.buckets.emplace(key, std::move(bucket)).first;
        }
        bucket_ptr = it->second.get();
    }   // shard lock released here
    return bucket_ptr->tryAcquire(tokens);   // only per-bucket lock held
}
3.3 Why Buckets Are Never Erased — and Why the Pointer Stays Valid
After the shard lock is released, we still hold bucket_ptr and call into it. This is safe only because buckets are never erased. If we deleted buckets (e.g., an LRU eviction to bound memory), the pointer could dangle — a use-after-free.

This is a deliberate memory-vs-safety tradeoff:

Pro: Simple, lock-free pointer use after the shard lock is released; different keys in the same shard progress in parallel.

Con: Unbounded memory growth if the key space is unbounded.

Production hardening you could add: periodic eviction under the shard lock, using shared_ptr instead of unique_ptr so existing holders keep the object alive while it is removed from the map.

3.4 Hash Distribution Quality
std::hash<std::string> is implementation-defined and not cryptographically random. For rate limiting this is fine — you need uniform distribution, not unpredictability. However, an adversary who can choose keys and knows the hash function could craft keys that all land in one shard (hash flooding), degrading to single-lock performance.

Mitigation: use a seeded hash (e.g., std::hash<std::string> with a per-process random seed) or a keyed hash such as SipHash.

3.5 Redis Backend — Why Distributed State Is Hard
Multiple server instances must share rate-limit state. Redis is the natural store. The naive implementation:

cpp
// WRONG — three separate round trips
auto tokens = redis->hget(key, "tokens");       // RT 1
auto last   = redis->hget(key, "last_refill");  // RT 2
// ... compute locally ...
redis->hset(key, "tokens", new_tokens);         // RT 3
Race: Server A and Server B both read tokens = 1. Both compute "allowed." Both write tokens = 0. Two requests allowed, one token consumed. This is the distributed version of the lost update.

Even wrapping this in a Redis MULTI/EXEC transaction does not help, because the decision (allow vs reject) depends on the read value, and Redis transactions do not support conditional logic — you cannot say "abort if tokens < 1."

3.6 The Lua Script — Atomic Decision + Mutation
The solution is to move the entire read-refill-decide-write sequence into Redis as a single Lua script. Redis executes Lua atomically: no other command runs during script execution.

lua
-- KEYS[1] = bucket key
-- ARGV[1] = capacity
-- ARGV[2] = refill rate (tokens/sec)
local key          = KEYS[1]
local capacity     = tonumber(ARGV[1])
local refill_rate  = tonumber(ARGV[2])

-- Time comes from Redis, NOT the client
local t = redis.call("TIME")
local now = tonumber(t[1]) + tonumber(t[2]) / 1000000

local b = redis.call("HMGET", key, "tokens", "last_refill")
local tokens      = tonumber(b[1])
local last_refill = tonumber(b[2])
if tokens == nil then
    tokens = capacity
    last_refill = now
end

local elapsed = now - last_refill
if elapsed > 0 then
    tokens = math.min(capacity, tokens + elapsed * refill_rate)
    last_refill = now
end

local allowed = 0
if tokens >= 1 then
    tokens = tokens - 1
    allowed = 1
end

redis.call("HSET", key, "tokens", tokens, "last_refill", last_refill)
redis.call("EXPIRE", key, 3600)
return allowed
Three critical details:

1. Server-side time. redis.call("TIME") reads Redis's own clock. If each application server used its own clock, two servers with clocks offset by 500 ms would compute different refill amounts for the same bucket, leading to over- or under-granting. Redis is the single source of truth for time.

2. EXPIRE for garbage collection. Without it, every distinct client key would live in Redis forever — a memory leak. The 3600-second TTL means an idle bucket is reclaimed after an hour.

3. Everything in one script. No other client can interleave between the HMGET and the HSET.

3.7 EVALSHA and the NOSCRIPT Recovery Path
Sending the full script text on every request wastes bandwidth. Instead:

On startup, SCRIPT LOAD uploads the script and returns a SHA1 hash.

On each request, EVALSHA <sha> ... executes it, sending only 40 bytes.

The failure mode: Redis can lose loaded scripts on restart, failover, or SCRIPT FLUSH. Then EVALSHA returns a NOSCRIPT error.

cpp
try {
    long long allowed = redis_->evalsha(script_sha_, {key}, {cap, rate});
    return allowed == 1;
} catch (const sw::redis::Error& e) {
    const std::string msg = e.what();
    if (msg.find("NOSCRIPT") == std::string::npos) {
        // NOT a NOSCRIPT error — do NOT retry
        std::cerr << "RedisBackend: denying request: " << msg << '\n';
        return false;
    }
    std::lock_guard<std::mutex> lock(script_mutex_);
    script_sha_ = loadScriptSha();      // re-upload, get new SHA
    long long allowed = redis_->evalsha(script_sha_, {key}, {cap, rate});
    return allowed == 1;
}
Why retry only on NOSCRIPT: A timeout or connection error is ambiguous — the command may have executed but the response was lost. Retrying could double-consume a token, granting a request that was already denied, or double-denying. NOSCRIPT is unambiguous: the script never ran, so retrying is safe.

Fail-closed policy: Any other Redis error results in return false — the request is denied. This is a deliberate choice. Failing open would let an outage disable rate limiting entirely, turning a degraded state into a security incident.

PAGE 4 — Leader Election, Fencing, Testing, Benchmarks
4.1 Why a Leader at All?
Some operations must be performed by exactly one process — periodic aggregation, cache warming, scheduled cleanup. Running them on every instance causes duplicate work or corruption. The project includes a Redis-based leader election so one instance can be designated "the leader" and others stand by.

4.2 The Naive Implementation and Its Race
cpp
// Naive: SET key value NX EX ttl
bool acquired = redis->set(election_key_, node_id_, ttl, NOT_EXIST);
The leader holds the key and renews the TTL periodically. If it crashes, the TTL expires and another node can claim leadership.

The race — stale leader resumption:

Node A acquires the lease (TTL = 10s).

Node A suffers a long GC pause / VM freeze for 15 seconds.

Redis expires Node A's key at t=10s.

Node B acquires the lease at t=11s. Node B is now legitimately the leader.

Node A wakes up at t=15s and blindly renews: EXPIRE key 10.

Node A has now extended Node B's lease thinking it is its own. Node A believes it is the leader and performs leader-only work.

Result: two leaders simultaneously — split-brain. The leader-only work runs twice, potentially corrupting state.

4.3 Fencing Tokens — the Fix
Each leadership term gets a unique token:

cpp
std::string generateLeaseToken(const std::string& node_id) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    return node_id + ":" + std::to_string(dist(gen));
}
Renewal is only allowed if the caller's token still matches what Redis holds:

lua
-- leader_renew.lua
local key         = KEYS[1]
local token       = ARGV[1]
local ttl_seconds = tonumber(ARGV[2])

local current = redis.call("GET", key)
if current == token then
    redis.call("EXPIRE", key, ttl_seconds)
    return 1
else
    return 0
end
Now replay the race:

Node A acquires with token A:8271, TTL 10s.

Node A freezes for 15s.

Key expires. Node B acquires with token B:3390.

Node A wakes and calls the renew script with A:8271.

Script reads current = "B:3390", compares to "A:8271" → mismatch → returns 0.

Node A learns it is no longer leader (is_leader_ = false) and stops doing leader work.

Fencing converts a silent split-brain into a safe, detected demotion.

4.4 Memory Ordering in is_leader_
cpp
std::atomic<bool> is_leader_;
// ...
is_leader_.store(true, std::memory_order_release);
is_leader_.store(false, std::memory_order_release);
// ...
bool leader = is_leader_.load(std::memory_order_acquire);
The release/acquire pairing ensures that any state the leader wrote before announcing leadership is visible to a thread that observes is_leader_ == true. Without this, a compiler or CPU could reorder the flag write ahead of the state writes, and another thread could see "leader = true" but stale data.

4.5 Testing Strategy
Unit tests (CTest, run in CI):

Test file	What it validates
test_token_bucket.cpp	Initial tokens = capacity; consumption; refill over time; rejection when empty; boundary conditions; invalid constructor args
test_sharded_rate_limiter.cpp	Per-key isolation; concurrent access; capacity enforcement per key; shard distribution
test_redis_backend.cpp	Connectivity; atomic consumption; concurrent access; multi-key isolation; NOSCRIPT recovery
test_leader_elector.cpp	Acquisition; renewal; expiry; competing leaders; stale-leader rejection
Manual tests (not in default CTest — require live Redis):

manual_redis_backend_test.cpp — real Redis, real Lua, real latency

manual_leader_election_test.cpp — wall-clock timestamp verification of TTL expiry

manual_multi_key_stress_test.cpp — many keys under concurrent load

manual_stress_test.cpp — single-key contention

redis_smoke_test.cpp — basic connectivity sanity check

The concurrency demo (concurrency_demo_main.cpp) is the most valuable correctness artifact. It runs five deterministic scenarios:

Single-key contention: 32 threads × 50 attempts, capacity 100, refill 0. Expected: exactly 100 grants. Any other number proves a race.

Independent keys: 16 keys, 4 threads/key, 20 attempts each, capacity 10. Expected: each key grants exactly 10.

Redis atomicity: multiple threads on one Redis key. Expected: total grants = capacity, no double-spend.

Multi-key isolation: one key's exhaustion does not affect another.

Leader election & TTL: verify lease acquisition, renewal, and expiry behavior.

Scenario 1 is the killer test — it is deterministic. With capacity 100 and zero refill, the number of grants must be exactly 100, regardless of scheduling. Flaky results always indicate a real concurrency bug.

4.6 Benchmarks
bench_main.cpp — in-memory throughput and latency percentiles

Reference numbers from the README (single development machine, not a guarantee):

Configuration	Throughput	Avg	P50	P95	P99	Speedup
Single bucket	5.24 M req/s	2.45 µs	0.20 µs	4.60 µs	28.90 µs	1.00×
4 shards	13.32 M req/s	0.91 µs	0.30 µs	2.10 µs	6.40 µs	2.54×
16 shards	15.21 M req/s	0.74 µs	0.30 µs	0.90 µs	2.20 µs	2.91×
64 shards	19.59 M req/s	0.56 µs	0.30 µs	0.50 µs	1.30 µs	3.74×
Reading these correctly: the dramatic improvement is in the tail (P99 drops from 28.90 µs to 1.30 µs — a 22× reduction). This is the real story. Throughput doubles, but tail latency collapses. In production, P99 is what users feel and what triggers timeouts. Sharding is a tail-latency optimization as much as a throughput optimization.

bench_redis_main.cpp — 2,237.51 req/s, P50 442.20 µs, P95 21.15 ms, P99 51.22 ms. The ~200× gap versus in-memory is the cost of a network round trip. This is why you cache local decisions and use Redis only where distributed coordination is genuinely required.

bench_contention_main.cpp — intentionally amplifies contention with a simulated critical section. Global lock ~1,989 req/s; 64 shards ~23,397 req/s. This isolates the effect of sharding from the algorithm itself.

docs/benchmark-methodology.md documents how measurements are taken, which matters because a benchmark without methodology is marketing.

4.7 CI/CD
.github/workflows/ci.yml — three jobs:

Linux (GCC): full build, all tests, concurrency demo, benchmark smoke test, Redis service container

Windows (MSVC): build + non-Redis unit tests

ThreadSanitizer (Clang + TSan): build and test with TSan enabled — this is the job that catches data races that functional tests miss

.github/workflows/benchmark.yml — weekly scheduled runs (Monday 02:30 UTC) plus manual dispatch, uploading benchmark CSV as a build artifact. This creates a performance history so regressions are visible.

4.8 Build & Run
bash
# 1. Clone
git clone https://github.com/Dikshu015/distributed-rate-limiter
cd distributed-rate-limiter

# 2. Start Redis
docker compose up -d redis

# 3. Configure (vcpkg pulls hiredis + redis-plus-plus)
cmake --preset default

# 4. Build
cmake --build build --config Release

# 5. Test
cd build && ctest --output-on-failure

# 6. Run the correctness demo
./concurrency_demo

# 7. Run benchmarks
./bench
./bench_redis
./bench_contention

# 8. Run the server demo
NODE_ID=node_a REDIS_URL=tcp://127.0.0.1:6379 ./server
PAGE 5 — Design Tradeoffs, Limitations, and Learning Path
5.1 Every Design Decision Has a Cost
Decision	Benefit	Cost / Limitation
Lazy refill (no timer thread)	O(1), no idle CPU, no timer races	Token count is only accurate when queried
Non-thread-safe TokenBucket	Clean algorithm tests	Caller must use the wrapper
Single mutex in ThreadSafeTokenBucket	Simple, obviously correct	Full serialization per bucket
Sharding by hash(key) % N	~3.7× throughput, 22× tail improvement	Uneven keys → hot shards; hash flooding possible
Fixed shard count	No rehashing complexity	Cannot resize at runtime
Buckets never erased	Safe pointer use after shard unlock	Unbounded memory for unbounded keys
Lua script for atomicity	Correct across all servers	Script complexity; must handle NOSCRIPT
Redis server-side TIME	No clock skew bugs	Requires Redis ≥ 3.2; couples to Redis clock
Fail-closed on Redis errors	Security preserved during outages	Availability sacrificed — requests denied
EXPIRE 3600s on buckets	Automatic GC	A bucket idle > 1h resets to full capacity
Fencing tokens in leader election	Prevents split-brain	Requires an extra GET compare per renewal
5.2 Known Limitations (Honest Assessment)
Hash flooding. std::hash<std::string> is not keyed. A determined adversary could degrade sharding to single-lock performance. Fix: keyed hash or per-process seed.

Unbounded bucket growth. No eviction. In a system with millions of distinct keys and no TTL, memory grows without bound. Fix: shared_ptr + periodic eviction under the shard lock.

Fixed shard count. Chosen at construction. Resizing requires a new limiter and state migration.

EXPIRE resets buckets. A client idle for 3600s gets a fresh full-capacity bucket. Usually acceptable; document it.

Fail-closed availability. During a Redis outage, all rate-limited requests are denied. Some systems prefer fail-open with alerting. This is a policy decision, not a bug — but it must be a conscious one.

Single Redis instance. No Redis Cluster / Sentinel support. A Redis failover triggers NOSCRIPT recovery, but a full outage denies all traffic.

No sliding-window or leaky-bucket variant. Token bucket only. Different algorithms suit different traffic shapes.

Tokens-per-request is not persisted. The Lua script consumes exactly 1 token. Multi-token costs are supported in the in-memory path but the script hardcodes 1.

5.3 What This Project Teaches
This is a compact but unusually complete systems project. It demonstrates:

Algorithm design — token bucket, lazy refill, burst vs sustained rate

Concurrency fundamentals — TOCTOU, lost updates, critical sections, memory ordering

Scalability technique — sharding to reduce lock contention, and reading tail latency

Distributed systems — why local state fails, atomic operations via Lua, fail-closed vs fail-open

Coordination — leader election, lease TTLs, and the fencing problem

Engineering rigor — deterministic concurrency tests, TSan in CI, documented benchmarks

The fencing-token section is the deepest material. Most engineers implement SET NX EX and stop. Understanding why that is insufficient — the paused-leader race — is what distinguishes a senior-level answer.

5.4 Suggested Learning Order and Time
Step	File(s)	Time
1	README.md — architecture, modes, config	1–2 h
2	include/token_bucket.h, src/token_bucket.cpp	2–3 h
3	thread_safe_token_bucket.h/.cpp — TOCTOU, critical sections	1–2 h
4	sharded_rate_limiter.h/.cpp — two-level locking	2–3 h
5	scripts/token_bucket.lua, redis_backend.h/.cpp — atomicity, NOSCRIPT	3–4 h
6	leader_elector.h/.cpp, scripts/leader_renew.lua — fencing	3–4 h
7	Build, run ctest, run concurrency_demo	2–4 h
8	Run all three benchmarks, compare to README	1–2 h
9	Modify: add multi-token Lua support or bucket eviction	1–2 days
Total with prerequisites met: 2–3 focused days.
Total including Redis/Lua/concurrency brush-up: 1–2 weeks.

The single highest-value exercise: make the Lua script support ARGV[3] = tokens_requested and add a test that verifies a 5-token request against a 3-token bucket is rejected atomically. That forces you to touch every layer of the project.

APPENDIX — 20 Interview Questions and Answers
Q1. What is a token bucket, and why use it over a fixed-window counter?
A token bucket holds up to capacity tokens, refilled at refill_rate tokens/sec. Each request consumes tokens (usually 1). It answers "allow or deny" in O(1).

A fixed-window counter resets at the top of each window. It allows a 2× burst at boundaries: 100 requests at 11:59:59 and 100 at 12:00:00 means 200 requests in two seconds while still "respecting" a 100/minute limit. A token bucket smooths this because refill is continuous — there is no boundary to exploit. It also cleanly separates burst tolerance (capacity) from sustained rate (refill_rate), which fixed windows cannot express.

Q2. Why is TokenBucket not thread-safe, and how do you make it safe?
It is deliberately not thread-safe to keep the algorithm testable in isolation — a failure in TokenBucket tests is unambiguously an algorithm bug, never a concurrency bug. Concurrency is layered on with ThreadSafeTokenBucket, which wraps it in a std::mutex and locks for the entire tryAcquire call. This separation is a deliberate testing and debugging strategy.

Q3. Why must the entire refill-check-decrement be one critical section?
Because it is a compound operation on shared state. If you lock only the decrement, two threads can both read tokens = 1, both pass if (tokens >= 1), and both decrement — the bucket goes negative and two requests are granted when only one token existed. This is a TOCTOU race combined with a lost update. One lock must cover read-modify-write as an indivisible unit.

Q4. Why does the token bucket use lazy refill instead of a background timer?
A timer thread means either one thread per bucket (unacceptable at scale) or a global timer thread with a lock (a contention bottleneck), plus a whole class of timer-vs-request races. Lazy refill computes tokens from elapsed time at the moment of the query — O(1), zero idle cost, and no extra synchronization. The tradeoff is that the token count is only accurate when queried, which is fine because that is the only time it matters.

Q5. Why steady_clock and not system_clock?
steady_clock is monotonic — it never jumps backward. system_clock can be adjusted by NTP or a manual clock change, producing negative elapsed time. A negative elapsed would make the refill computation either subtract tokens or (if clamped) stall the bucket indefinitely. For measuring durations, always use steady_clock.

Q6. Explain the sharding strategy and why it helps.
Keys are hashed to a shard index: hash(key) % num_shards. Each shard has its own mutex and its own map of buckets. Without sharding, one global lock serializes every request regardless of client. With sharding, requests for different keys usually land in different shards and never contend. The repo's benchmarks show roughly 3.7× throughput and a ~22× P99 improvement at 64 shards.

Q7. Why are buckets never erased, and what is the risk?
The tryAcquire path releases the shard-map lock before calling into the bucket, relying on the bucket pointer remaining valid. If buckets were erased, that pointer could dangle — a use-after-free. Never erasing guarantees validity and lets different keys in the same shard proceed in parallel. The risk is unbounded memory growth if the key space is unbounded. A production fix uses shared_ptr (so in-flight holders keep the object alive) plus periodic eviction under the shard lock.

Q8. What is hash flooding, and does it affect this design?
Yes. std::hash<std::string> is deterministic and not keyed, so an adversary who can choose keys and knows the hash function can craft many keys that collide into the same shard, degrading the sharded limiter back to single-lock performance — a denial of service. Mitigation: use a keyed hash (SipHash) or a per-process random seed so the adversary cannot predict the mapping.

Q9. Why can't you just use Redis MULTI/EXEC for the distributed limiter?
Redis transactions queue commands and execute them atomically, but they cannot make decisions based on read values. The rate limiter needs conditional logic: "refill, then if tokens ≥ 1, decrement and allow; else deny." MULTI/EXEC cannot express "abort or branch based on a previously read value." Lua can, because it runs arbitrary logic server-side atomically. You could use WATCH/MULTI optimistic locking, but it would retry under contention, whereas Lua executes once with no retry loop.

Q10. Why does the Lua script use Redis's TIME instead of client time?
Because all servers share one bucket. If each server used its own clock, two servers offset by even a few hundred milliseconds would compute different refill amounts for the same bucket, causing over- or under-granting depending on which server handled the request. Redis is a single source of truth for time, so every server computes the same refill for the same bucket.

Q11. What does EXPIRE on the bucket key accomplish?
Garbage collection. Without it, every distinct client key ever seen would live in Redis forever — an unbounded memory leak. A 3600-second TTL reclaims buckets for clients that have gone idle. The side effect is that a client idle longer than the TTL gets a fresh full-capacity bucket, which is usually acceptable and should be documented.

Q12. Explain SCRIPT LOAD and EVALSHA, and the NOSCRIPT failure.
SCRIPT LOAD uploads a Lua script to Redis and returns its SHA1. EVALSHA <sha> executes it by hash, sending 40 bytes instead of the whole script. Redis can lose loaded scripts on restart, failover, or SCRIPT FLUSH, after which EVALSHA returns NOSCRIPT. The fix is to catch that specific error, re-upload with SCRIPT LOAD, and retry once.

Q13. Why is it dangerous to retry on any Redis error rather than only NOSCRIPT?
Because non-NOSCRIPT errors are ambiguous. A timeout or connection reset means the command may have executed successfully but the response was lost. Retrying could consume a second token for a request that already succeeded — a double-spend — or grant a request that was already denied. NOSCRIPT is unambiguous: the script demonstrably never ran, so retrying is safe. This is the general principle of only retrying on errors that are provably safe to retry.

Q14. The limiter fails closed on Redis errors. Argue for and against.
For (fail-closed): Rate limiting is often a security control. Failing open during a Redis outage means an attacker who can degrade Redis can disable rate limiting entirely, converting an availability incident into an abuse incident. Denying requests preserves the security property.

Against (fail-open): A Redis outage then becomes a full outage for the protected service — a small infrastructure failure cascades into a large one. Many production systems prefer fail-open with aggressive alerting, on the theory that a brief window of unlimited traffic is less bad than total downtime.

The right answer depends on what the limiter protects. Abuse prevention (auth attempts, password resets) → fail closed. Fairness or cost control on a public API → fail open with alerting. The key point is that it must be a deliberate, documented decision, not an accident of implementation.

Q15. Describe the leader election mechanism.
A Redis key with a TTL represents the lease. A node acquires it with SET key <token> NX EX <ttl>. The holder renews by extending the TTL. If the holder dies, the key expires and another node can acquire it. Each leadership term has a unique token — node_id + ":" + random — so terms are distinguishable.

Q16. What is the fencing problem in leader election?
A leader can be paused (GC, VM freeze, slow disk) for longer than its TTL. Redis expires its lease and another node becomes leader. When the first node wakes, it may blindly renew the TTL — but the key now belongs to the new leader. The first node has extended someone else's lease while believing it is still leader. Result: two leaders simultaneously (split-brain), and leader-only work runs twice, potentially corrupting state.

Q17. How does the fencing token solve it?
Renewal is conditional. The renew Lua script reads the current key value and extends the TTL only if it matches the caller's token. A stale leader presenting its old token gets a mismatch, the renewal fails, and it sets is_leader_ = false and stops doing leader work. The critical property: the compare-and-renew happens atomically inside Redis, so there is no window between the check and the extension. Fencing converts a silent split-brain into a detected, safe demotion.

Q18. Why are std::atomic loads/stores in LeaderElector given explicit memory orders?
The is_leader_ flag is published from the thread that renews and read by threads that decide whether to do leader work. A release store paired with an acquire load guarantees that any state the leader wrote before setting the flag is visible to a thread that observes the flag as true. With memory_order_relaxed, the compiler or CPU could reorder the flag write ahead of the data writes, and a reader could see "leader = true" while reading stale state. The default seq_cst is also correct but stricter than needed; release/acquire expresses the actual requirement.

Q19. How do you test a concurrent rate limiter?
Three layers. Deterministic concurrency tests: e.g., capacity 100, zero refill, 32 threads × 50 attempts — the result must be exactly 100 grants. Zero refill makes the expected value independent of scheduling, so any other number proves a race. ThreadSanitizer in CI: catches data races functional tests miss. Stress tests with live Redis: validate real network and timing behavior. The combination is what makes the correctness claim credible — a functional test alone cannot prove absence of races.

Q20. How would you extend this to multi-token requests and to bounded memory?
Multi-token requests: the in-memory path already accepts a tokens parameter, but the Lua script hardcodes tokens >= 1. Add ARGV[3] as the requested token count, change the check to tokens >= requested, and decrement by requested. Add a test that a 5-token request against a 3-token bucket is rejected atomically and leaves the bucket unchanged.

Bounded memory: change std::unique_ptr to std::shared_ptr in the shard map so that a thread holding a pointer after releasing the shard lock keeps the bucket alive even if it is evicted. Then add a background eviction pass that, under each shard's lock, removes buckets idle beyond a threshold. Alternatively, lean on Redis EXPIRE for the distributed path and apply the same TTL concept to local buckets with a last-access timestamp. The shared_ptr change is essential — evicting with unique_ptr would reintroduce the use-after-free the current design deliberately avoids.