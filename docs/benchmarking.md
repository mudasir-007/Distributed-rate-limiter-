# Benchmarking Guide

The project intentionally keeps benchmarks dependency-light and reports raw measurements rather than hard-coded performance claims.

## Metrics

| Metric | Meaning |
|---|---|
| Requests | Total operations attempted |
| Granted | Operations for which the limiter returned `true` |
| Denied | Operations for which the limiter returned `false` |
| Elapsed | Wall-clock time for the complete benchmark |
| Throughput | `requests / elapsed_seconds` |
| Average latency | Arithmetic mean of per-request latency |
| P50 | Median latency |
| P95 | 95th percentile latency |
| P99 | 99th percentile latency |
| Min | Fastest measured request |
| Max | Slowest measured request |

## Recommended procedure

Run the same command at least three times on an otherwise idle machine.

Record:

- CPU model and core count;
- OS version;
- compiler/version;
- Release/Debug configuration;
- thread count;
- requests per thread;
- number of keys;
- Redis version for Redis tests;
- Redis topology and connection settings.

For latency-sensitive analysis, report P95/P99 alongside throughput. Average latency can hide tail latency.

## In-memory benchmark

```bash
./build/bench 16 10000 1000 build/benchmark_results.csv
```

The benchmark compares one shared thread-safe bucket with sharded configurations.

## Redis benchmark

```bash
docker compose up -d redis
./build/bench_redis tcp://127.0.0.1:6379 8 1000 1000000000
```

This measures the actual Redis/Lua backend, including client/server communication.

## Contention benchmark

```bash
./build/bench_contention 16 200 1000 500
```

This intentionally keeps simulated work inside a lock. Its purpose is to demonstrate how partitioning work across shards changes contention behavior.

It should not be interpreted as a production latency model.

## CSV output

`bench` accepts an optional fourth argument:

```bash
./build/bench 16 10000 1000 build/benchmark_results.csv
```

The CSV columns are:

```text
scenario
requests
granted
denied
elapsed_ms
throughput_req_s
avg_us
p50_us
p95_us
p99_us
min_us
max_us
```

Benchmark output is ignored by Git through `.gitignore`.
