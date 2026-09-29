-- Atomic token-bucket operation.
--
-- KEYS[1] = bucket key
-- ARGV[1] = capacity
-- ARGV[2] = refill rate (tokens/second)
--
-- Time comes from Redis itself rather than the client. This prevents two
-- application processes with different system clocks from calculating
-- different refill amounts for the same distributed bucket.
--
-- Returns 1 when one token is consumed, otherwise 0.

local key = KEYS[1]
local capacity = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])

local server_time = redis.call("TIME")
local now = tonumber(server_time[1]) + tonumber(server_time[2]) / 1000000

local bucket = redis.call("HMGET", key, "tokens", "last_refill")
local tokens = tonumber(bucket[1])
local last_refill = tonumber(bucket[2])

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
