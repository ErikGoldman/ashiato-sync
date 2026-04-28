# Kage Sync Guide

Kage Sync is a separate project from the kagesoko ECS system at `../main`. Implement sync code, tests, and benchmarks here unless a measured ECS limitation requires a kagesoko change, in which case suggest changes to kagesoko's internals or API that might help.

The project is a library for fixed-tick, predictive networking for online games. There is a server and multiple clients, entities are marked as replicated with replication settings, and the server periodically sends updates down to the clients. On the client, entities can be in snap mode (immediately move or interpolate to the new state), buffered interpolate (store multiple frames and interpolate between them), or predict (extrapolate from the last received frame and, upon receipt of a server frame, potentially roll back and resimulate).

This is under active development with no users, so never prioritize backwards compatibility.

## Performance

This is a high-performance project. Always consider the performance effect of any change on all possible operations and suggest additions to the benchmark suite to measure the impact of any significant change. When designing a new feature, carefully consider how to optimize for cache hits, avoiding memory allocation churn, and keeping the hot paths fast.

## Benchmarks

- Put scheduler/server benchmarks in this project behind `KAGE_SYNC_BUILD_BENCHMARKS`.
- Build benchmark numbers with `RelWithDebInfo`.
- Run benchmark configurations serially. Never overlap long benchmark jobs when collecting numbers.
- Prefer focused benchmark filters for confirmation runs.
- Record the command and artifact path used in any benchmark summary.
- Treat Debug timings as smoke tests, not authoritative performance data.
- Use gprof to find hot spots

## Optimization

- Previous optimization experiments are in OPTIMIZATION\_EXPERIMENTS.md -- read it for context and update it when trying new experiments.
