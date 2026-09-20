#include "allocation_tracker.hpp"
#include "benchmark_helpers.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using namespace ashiato::sync::benchmarks;

class ScopedAllocationTracking {
public:
    ScopedAllocationTracking() noexcept {
        reset_allocation_counts();
        set_allocation_tracking(true);
    }

    ScopedAllocationTracking(const ScopedAllocationTracking&) = delete;
    ScopedAllocationTracking& operator=(const ScopedAllocationTracking&) = delete;

    ~ScopedAllocationTracking() {
        set_allocation_tracking(false);
    }
};

void merge_maximums(AllocationCounts& maximums, const AllocationCounts& sample) {
    maximums.allocations = std::max(maximums.allocations, sample.allocations);
    maximums.allocated_bytes = std::max(maximums.allocated_bytes, sample.allocated_bytes);
    maximums.live_allocations = std::max(maximums.live_allocations, sample.live_allocations);
    maximums.live_bytes = std::max(maximums.live_bytes, sample.live_bytes);
    maximums.peak_live_allocations = std::max(maximums.peak_live_allocations, sample.peak_live_allocations);
    maximums.peak_live_bytes = std::max(maximums.peak_live_bytes, sample.peak_live_bytes);
}

void report_memory(benchmark::State& state, const AllocationCounts& counts, int entity_count) {
    state.counters["allocation_calls"] = static_cast<double>(counts.allocations);
    state.counters["allocated_bytes"] = static_cast<double>(counts.allocated_bytes);
    state.counters["peak_live_allocations"] = static_cast<double>(counts.peak_live_allocations);
    state.counters["peak_live_bytes"] = static_cast<double>(counts.peak_live_bytes);
    state.counters["retained_allocations"] = static_cast<double>(counts.live_allocations);
    state.counters["retained_bytes"] = static_cast<double>(counts.live_bytes);
    state.counters["retained_bytes/entity"] =
        static_cast<double>(counts.live_bytes) / static_cast<double>(entity_count);
}

void BM_MemoryServerSteadyState(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));
    AllocationCounts maximums;

    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        {
            ScopedAllocationTracking tracking;
            ashiato::Registry registry;
            const ashiato::sync::SyncArchetypeId archetype = define_delta_archetype(registry);
            ashiato::sync::ReplicationServerOptions options;
            options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * 128U;
            options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
            ashiato::sync::ReplicationServer server(registry, options);
            add_clients(server, client_count);
            {
                const std::vector<ashiato::Entity> entities = create_delta_entities(registry, entity_count);
                add_replication_configs(registry, entities, archetype);
            }
            server.rediscover_all_replicated_entities(registry);
            server.tick(registry, server.options().fixed_dt_seconds);
            merge_maximums(maximums, allocation_counts());
            benchmark::DoNotOptimize(server.replicated_count());
        }
        state.ResumeTiming();
        benchmark::ClobberMemory();
    }

    report_memory(state, maximums, entity_count);
}

void measure_client_memory(
    benchmark::State& state,
    ashiato::sync::ReplicationClientMode mode,
    bool interpolate) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);
    AllocationCounts maximums;

    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        {
            ScopedAllocationTracking tracking;
            ashiato::Registry registry;
            define_client_delta_schema(registry, interpolate);
            ashiato::sync::ReplicationClient client(registry, make_client_options(mode));
            for (const ashiato::BitBuffer& packet : packets) {
                benchmark::DoNotOptimize(client.receive(registry, packet));
            }
            merge_maximums(maximums, allocation_counts());
            benchmark::DoNotOptimize(client.pending_ack_count());
        }
        state.ResumeTiming();
        benchmark::ClobberMemory();
    }

    report_memory(state, maximums, entity_count);
}

void BM_MemoryClientSnapSteadyState(benchmark::State& state) {
    measure_client_memory(state, ashiato::sync::ReplicationClientMode::Snap, false);
}

void BM_MemoryClientBufferedSteadyState(benchmark::State& state) {
    measure_client_memory(state, ashiato::sync::ReplicationClientMode::BufferedInterpolation, true);
}

void BM_MemoryClientPredictSteadyState(benchmark::State& state) {
    measure_client_memory(state, ashiato::sync::ReplicationClientMode::Predict, false);
}

BENCHMARK(BM_MemoryServerSteadyState)->Args({16384, 8})->Iterations(1);
BENCHMARK(BM_MemoryClientSnapSteadyState)->Args({4096, 16})->Iterations(1);
BENCHMARK(BM_MemoryClientBufferedSteadyState)->Args({4096, 16})->Iterations(1);
BENCHMARK(BM_MemoryClientPredictSteadyState)->Args({4096, 16})->Iterations(1);

}  // namespace
