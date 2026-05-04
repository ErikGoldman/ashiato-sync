#include "benchmark_helpers.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace kage::sync::benchmarks;

void BM_ClientReceiveSnap(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::Snap,
            2,
            64});
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceiveBufferedInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            2,
            64});
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceivePredict(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClientOptions options;
        options.mtu_bytes = 1200;
        options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
        options.prediction_buffer_capacity_frames = 64;
        kage::sync::ReplicationClient client(options);
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientReceiveMixedEntityModes(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::ReplicationClientOptions options;
        options.mtu_bytes = 1200;
        options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
        options.interpolation_buffer_frames = 2;
        options.interpolation_buffer_capacity_frames = 64;
        options.entity_mode_selector = [](const kage::sync::ReplicatedEntityUpdateView& update) {
            return (kage::sync::client_entity_network_id_wire_id(update.client_entity_network_id) & 1U) == 0U
                ? kage::sync::ReplicationClientMode::BufferedInterpolation
                : kage::sync::ReplicationClientMode::Snap;
        };
        kage::sync::ReplicationClient client(std::move(options));
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientApplyBufferedInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (int frame = 2; frame < frame_count + 2; ++frame) {
            benchmark::DoNotOptimize(client.apply_frame(registry, static_cast<kage::sync::SyncFrame>(frame)));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count));
}

void BM_ClientPredictTickQuantize(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, 1);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClientOptions options;
        options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
        kage::sync::ReplicationClient client(options);
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (int frame = 2; frame < frame_count + 2; ++frame) {
            (void)frame;
            benchmark::DoNotOptimize(client.tick(registry, client.options().fixed_dt_seconds));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count));
}

void BM_ClientSampleDisplayInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::set_display_interpolated<DeltaPosition>(registry);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        kage::sync::DisplayInterpolationSampleBuffer display;
        state.ResumeTiming();

        for (int frame = 1; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.sample_display_interpolation_target_frame(
                registry,
                static_cast<double>(frame) + 0.5,
                display));
            benchmark::DoNotOptimize(display.entities.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frame_count - 1));
}

void BM_ClientSampleDisplayLargePayload(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets =
        make_large_payload_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_large_payload_schema(registry);
        kage::sync::set_display_interpolated<LargePayload>(registry);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        kage::sync::DisplayInterpolationSampleBuffer display;
        state.ResumeTiming();

        for (int frame = 1; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.sample_display_interpolation_target_frame(
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
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    std::int64_t drained = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::Snap,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        const std::vector<ecs::BitBuffer> acks = client.drain_ack_packets();
        drained += static_cast<std::int64_t>(acks.size());
        benchmark::DoNotOptimize(drained);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientDrainDuplicateHeavyAckPackets(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    std::int64_t drained = 0;
    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::Snap,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        const std::vector<ecs::BitBuffer> acks = client.drain_ack_packets();
        for (const ecs::BitBuffer& ack : acks) {
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
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::Snap,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets.destroys) {
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
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets.destroys) {
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
        ecs::Registry registry;
        define_client_delta_schema(registry, false);
        kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::Snap,
            2,
            64});
        for (const ecs::BitBuffer& packet : packets.initial) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        state.ResumeTiming();

        for (const ecs::BitBuffer& packet : packets.destroys) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
        for (const ecs::BitBuffer& packet : packets.respawns) {
            benchmark::DoNotOptimize(client.receive(registry, packet));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 2);
}

void BM_ClientTickBufferedAutoInterpolation(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int frame_count = static_cast<int>(state.range(1));
    const std::vector<ecs::BitBuffer> packets = make_client_receive_packets(entity_count, frame_count);

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        define_client_delta_schema(registry, true);
        kage::sync::ReplicationClientOptions options;
        options.mtu_bytes = 1200;
        options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
        options.interpolation_buffer_frames = 2;
        options.interpolation_buffer_capacity_frames = 64;
        options.auto_interpolation_buffer_frames = true;
        options.fixed_dt_seconds = 1.0 / 60.0;
        kage::sync::ReplicationClient client(options);
        state.ResumeTiming();

        for (std::size_t packet_index = 0; packet_index < packets.size(); ++packet_index) {
            benchmark::DoNotOptimize(client.receive(
                registry,
                packets[packet_index],
                static_cast<kage::sync::SyncFrame>(packet_index + 3U),
                static_cast<kage::sync::SyncFrame>(packet_index)));
            benchmark::DoNotOptimize(client.tick(registry, 1.0 / 60.0));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(packets.size()));
}

void BM_ClientInputRecordAndDrain(benchmark::State& state) {
    const int frame_count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        ecs::Registry registry;
        kage::sync::register_sync_component<DeltaPosition>(registry, "DeltaPosition");
        kage::sync::configure_client(registry, 1);
        kage::sync::set_client_input_component<DeltaPosition>(registry);
        const ecs::Entity owned = registry.create();
        kage::sync::set_owner(registry, owned, 1);
        kage::sync::ReplicationClient client;
        state.ResumeTiming();

        for (int frame = 0; frame < frame_count; ++frame) {
            benchmark::DoNotOptimize(client.set_input(registry, DeltaPosition{frame, frame + 1}));
            benchmark::DoNotOptimize(client.tick(registry, client.options().fixed_dt_seconds));
        }
        std::vector<ecs::BitBuffer> packets = client.drain_packets();
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
BENCHMARK(BM_ClientSampleDisplayInterpolation)->Apply(ClientArgs);
BENCHMARK(BM_ClientSampleDisplayLargePayload)->Apply(ClientArgs);
BENCHMARK(BM_ClientDrainAckPackets)->Apply(ClientArgs);
BENCHMARK(BM_ClientDrainDuplicateHeavyAckPackets)->Args({1024, 64})->Args({4096, 64});
BENCHMARK(BM_ClientReceiveDestroySnap)->Apply(DestroyArgs);
BENCHMARK(BM_ClientReceiveDestroyBuffered)->Apply(DestroyArgs);
BENCHMARK(BM_ClientReceiveSpawnDestroyChurn)->Apply(DestroyArgs);
BENCHMARK(BM_ClientTickBufferedAutoInterpolation)->Apply(ClientArgs);
BENCHMARK(BM_ClientInputRecordAndDrain)->Arg(16)->Arg(64);

}  // namespace
