# Ashiato Sync Guide

Ashiato Sync is a separate project from the Ashiato ECS system. CMake fetches a pinned Ashiato revision from GitHub by default; pass `ASHIATO_SYNC_ASHIATO_SOURCE_DIR` to use a local checkout when needed. Implement sync code, tests, and benchmarks here unless a measured ECS limitation requires an Ashiato change, in which case suggest changes to Ashiato's internals or API that might help.

The project is a library for fixed-tick, predictive networking for online games. There is a server and multiple clients, entities are marked as replicated with replication settings, and the server periodically sends updates down to the clients. On the client, entities can be in snap mode (immediately move or interpolate to the new state), buffered interpolate (store multiple frames and interpolate between them), or predict (extrapolate from the last received frame and, upon receipt of a server frame, potentially roll back and resimulate).

This is under active development with no users, so never prioritize backwards compatibility.

## Gotchas

Entity ids/references are not stable across clients/the server. Use the entity reference system if you want to serialize a reference to another entity.

## Performance

This is a high-performance project. Always consider the performance effect of any change on all possible operations and suggest additions to the benchmark suite to measure the impact of any significant change. When designing a new feature, carefully consider how to optimize for cache hits, avoiding memory allocation churn, and keeping the hot paths fast.

## Benchmarks

- Put scheduler/server benchmarks in this project behind `ASHIATO_SYNC_BUILD_BENCHMARKS`.
- Build benchmark numbers with `RelWithDebInfo`.
- Run benchmark configurations serially. Never overlap long benchmark jobs when collecting numbers.
- Prefer focused benchmark filters for confirmation runs.
- Record the command and artifact path used in any benchmark summary.
- Treat Debug timings as smoke tests, not authoritative performance data.
- Use gprof to find hot spots

## Optimization

- Previous optimization experiments are in OPTIMIZATION\_EXPERIMENTS.md -- read it for context and update it when trying new experiments.
- Bandwidth optimization experiments are in BANDWIDTH\_EXPERIMENTS.md

## Fuzzing

- Update and rerun the defensive protocol fuzzer whenever protocol message IDs, packet fields, component/cue serialization, or receive-side validation changes.

## Trace viewer

There's a trace viewer in tools/ that can visually debug a trace capture. If you work on this tool and need to check for a successful UI change, run with `--control-socket [path].sock` to open a socket for remote automation. Commands are one per like, and include "move x y", "mouse\_down x y", "scroll x y", "screenshot [path]". Sleep briefly between inputs to allow the UI time to refresh and use the screenshots to debug your work.

## Linters

- Ashiato Sync CMake targets compile with strict warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` on GCC/Clang and `/W4 /WX` on MSVC.
- Run `scripts/lint.sh` before changing sync code. It configures a lint build with `ASHIATO_SYNC_ENABLE_CLANG_TIDY=ON` and `ASHIATO_SYNC_ENABLE_CPPCHECK=ON`, then builds the `ashiato_sync` target so both tools run through CMake.
- When linting from the plugin checkout, the script uses the bundled `../ashiato` and `../spdlog` sources to avoid fetching dependencies.
