-- leader_renew.lua
--
-- Fencing-safe renewal of a leadership lease. Only extends the lease's
-- TTL if the caller's token still matches what's currently stored —
-- preventing a leader that has already lost its lease (e.g. after a
-- long pause) from clobbering a new leader's legitimate claim.
--
-- KEYS[1] = election key
-- ARGV[1] = this node's lease token (must match what's stored to renew)
-- ARGV[2] = ttl_seconds (how long to extend the lease by)
--
-- Returns 1 if the renewal succeeded, 0 if this node no longer holds
-- the lease (someone else's token is stored instead).

local key = KEYS[1]
local token = ARGV[1]
local ttl_seconds = tonumber(ARGV[2])

local current_token = redis.call("GET", key)

if current_token == token then
    redis.call("EXPIRE", key, ttl_seconds)
    return 1
else
    return 0
end