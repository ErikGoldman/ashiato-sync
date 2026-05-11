#include "benchmark_helpers.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace ashiato::sync::benchmarks;

void BM_ClientReceiveSnap(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::Snap));
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceiveBufferedInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceivePredict(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClientOptions options;
        options.network.mtu_bytes = 1200;
        options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
        ashiato::sync::ReplicationClient client(registry, options);
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceiveMixedEntityModes(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClientOptions options;
        options.network.mtu_bytes = 1200;
        options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
        options.buffered.buffered_frame_lag = 2;
        options.entities.mode_selector = [](const ashiato::sync::ReplicatedEntityUpdateView& update) {
            return (ashiato::sync::client_entity_network_id_wire_id(update.client_entity_network_id) & 1U) == 0U
                ? ashiato::sync::ReplicationClientMode::BufferedInterpolation
                : ashiato::sync::ReplicationClientMode::Snap;
        };
        ashiato::sync::ReplicationClient client(registry, std::move(options));
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientApplyBufferedInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (int frame = 2; frame < frame_count + 2; ++frame) {
            benchmark::DoNotOptimize(client.apply_frame(registry, static_cast<ashiato::sync::SyncFrame>(frame)));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count));
}

void BM_ClientPredictTickQuantize(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, 1);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClientOptions options;
        options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
        ashiato::sync::ReplicationClient client(registry, options);
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (int frame = 2; frame < frame_count + 2; ++frame) {
            (void)frame;
            benchmark::DoNotOptimize(client.tick(registry, client.fixed_dt_seconds()));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count));
}

void BM_ClientSampleFractionalTick(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::set_fractional_tick_sampled<DeltaPosition>(registry);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        ashiato::sync::FractionalTickSampleBuffer display;
        state.ResumeTiming();

        for (int frame = 1; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.sample_fractional_tick_frame(
                registry,
                static_cast<double>(frame) + 0.5,
                display));
            benchmark::DoNotOptimize(display.entities.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count - 1));
}

void BM_ClientSampleFractionalTickLargePayload(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets =
        make_large_payload_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_large_payload_schema(registry);
        ashiato::sync::set_fractional_tick_sampled<LargePayload>(registry);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        ashiato::sync::FractionalTickSampleBuffer display;
        state.ResumeTiming();

        for (int frame = 1; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.sample_fractional_tick_frame(
                registry,
                static_cast<double>(frame) + 0.5,
                display));
            benchmark::DoNotOptimize(display.entities.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count - 1));
}

void BM_ClientDrainAckPackets(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    std::int64_t drained = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::Snap));
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        const std::vector<ashiato::BitBuffer> acks = client.drain_ack_packets();
        drained += static_cast<std::int64_t>(acks.size());
        benchmark::DoNotOptimize(drained);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientDrainDuplicateHeavyAckPackets(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    std::int64_t drained = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::Snap));
        for (const ashiato::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        const std::vector<ashiato::BitBuffer> acks = client.drain_ack_packets();
        for (const ashiato::BitBuffer& ack : acks) {
            drained += static_cast<std::int64_t>(ack.byte_size());
        }
        benchmark::DoNotOptimize(drained);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(frame_count));
}

void BM_ClientReceiveDestroySnap(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const DestroyPackets packets = make_destroy_packets(entity_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::Snap));
        for (const ashiato::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets.destroys) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count));
}

void BM_ClientReceiveDestroyBuffered(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const DestroyPackets packets = make_destroy_packets(entity_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
        for (const ashiato::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets.destroys) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count));
}

void BM_ClientReceiveSpawnDestroyChurn(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const ChurnPackets packets = make_churn_packets(entity_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, false);
        ashiato::sync::ReplicationClient client(registry, make_client_options(ashiato::sync::ReplicationClientMode::Snap));
        for (const ashiato::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ashiato::BitBuffer& packet : packets.destroys) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        for (const ashiato::BitBuffer& packet : packets.respawns) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 2);
}

void BM_ClientTickBufferedAutoInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClientOptions options;
        options.network.mtu_bytes = 1200;
        options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
        options.buffered.buffered_frame_lag = 2;
        options.buffered.auto_buffered_frame_lag = true;
        options.clock.fixed_dt_seconds = 1.0 / 60.0;
        ashiato::sync::ReplicationClient client(registry, options);
        state.ResumeTiming();

        for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
            benchmark::DoNotOptimize(client.receive(registry, packets[packet_index]));
            benchmark::DoNotOptimize(client.tick(registry, 1.0 / 60.0));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientTickCappedLargeDelta(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int backlog_frames = static_cast<int>(state.range(1));
    const int cap_frames = static_cast<int>(state.range(2));
    const std::vector<ashiato::BitBuffer> packets = make_client_receive_packets(entity_count, 1);

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        define_client_delta_schema(registry, true);
        ashiato::sync::ReplicationClientOptions options;
        options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
        options.clock.max_fixed_steps_per_tick = static_cast<std::uint32_t>(cap_frames);
        ashiato::sync::ReplicationClient client(registry, options);
        benchmark::DoNotOptimize(client.receive(registry, packets.front()));
        state.ResumeTiming();

        benchmark::DoNotOptimize(client.tick(
            registry,
            static_cast<double>(backlog_frames) * client.options().clock.fixed_dt_seconds));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(cap_frames));
}

void BM_ClientInputRecordAndDrain(benchmark::State& state) {
    const int frame_count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        ashiato::Registry registry;
        ashiato::sync::register_sync_component<DeltaPosition>(registry, "DeltaPosition");
        ashiato::sync::ReplicationClientOptions input_options;
        input_options.session.local_client = 1;
        ashiato::sync::set_client_input_component<DeltaPosition>(registry);
        const ashiato::Entity owned = registry.create();
        ashiato::sync::set_owner(registry, owned, 1);
        ashiato::sync::ReplicationClient client(registry, input_options);
        state.ResumeTiming();

        for (int frame = 0; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.set_input(registry, DeltaPosition{frame, frame + 1}));
            benchmark::DoNotOptimize(client.tick(registry, client.fixed_dt_seconds()));
        }
        std::vector<ashiato::BitBuffer> packets = client.drain_packets();
        benchmark::DoNotOptimize(packets.size());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count));
}

BENCHMARK(BM_ClientReceiveSnap)->Apply(ClientArgs);
BENCHMARK(BM_ClientReceiveBufferedInterpolation)->Apply(ClientArgs);
BENCHMARK(BM_ClientReceivePredict)->Apply(ClientArgs);
BENCHMARK(BM_ClientReceiveMixedEntityModes)->Apply(ClientArgs);
BENCHMARK(BM_ClientApplyBufferedInterpolation)->Apply(ClientArgs);
BENCHMARK(BM_ClientPredictTickQuantize)->Apply(ClientArgs);
BENCHMARK(BM_ClientSampleFractionalTick)->Apply(ClientArgs);
BENCHMARK(BM_ClientSampleFractionalTickLargePayload)->Apply(ClientArgs);
BENCHMARK(BM_ClientDrainAckPackets)->Apply(ClientArgs);
BENCHMARK(BM_ClientDrainDuplicateHeavyAckPackets)->Args({1024, 64})->Args({4096, 64});
BENCHMARK(BM_ClientReceiveDestroySnap)->Apply(DestroyArgs);
BENCHMARK(BM_ClientReceiveDestroyBuffered)->Apply(DestroyArgs);
BENCHMARK(BM_ClientReceiveSpawnDestroyChurn)->Apply(DestroyArgs);
BENCHMARK(BM_ClientTickBufferedAutoInterpolation)->Apply(ClientArgs);
BENCHMARK(BM_ClientTickCappedLargeDelta)->Args({1024, 64, 4})->Args({4096, 64, 4});
BENCHMARK(BM_ClientInputRecordAndDrain)->Arg(16)->Arg(64);

}  // namespace
