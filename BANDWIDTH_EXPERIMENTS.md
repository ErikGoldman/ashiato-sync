# Bandwidth Experiments

Benchmark scenario unless noted:

```sh
./build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

The `build-bench` configuration uses `RelWithDebInfo` and
`KAGE_SYNC_BUILD_BENCHMARKS=ON`.

Artifact: `build-bench/kage_sync_ball_stress`

## Baseline

Measured before the bandwidth experiments:

- `wall=14.418035`
- `server_replication=7.251542`
- `client_receive=5.729095`
- `ack_processing=1.180787`
- `server_to_clients.bytes=243959159`
- `clients_to_server.bytes=70297801`
- `clients_to_server.ack_records=5779055`
- `clients_to_server.ack_record_bits=560568335`
- `server_to_clients.delta_upsert_bits=1738966556`
- `server_to_clients.destroy_record_bits=3003910`

## Experiment 1: Packet-Level ACKs

Rationale: client ACK packets were dominated by per-entity records carrying
`destroy`, `frame`, and a 64-bit entity id. Server updates are already packetized,
and the server knows which entity records were included in each packet. This
experiment adds a 32-bit server packet id to update headers and changes client
ACK records to acknowledge packet ids. The server expands each packet ACK back
into the existing per-entity/destroy acknowledgement path.

Result:

- `wall=14.014714`
- `server_replication=7.103875`
- `client_receive=5.751302`
- `ack_processing=0.839655`
- `server_to_clients.bytes=244792834`
- `clients_to_server.bytes=795251`
- `clients_to_server.ack_records=196121`
- `clients_to_server.ack_record_bits=6275872`
- `server_to_clients.server_update_header_bits=18345976`

Conclusion: accepted. Upload ACK traffic dropped from `70297801` bytes to
`795251` bytes. Downstream grew by `833675` bytes because each server update
packet now carries a packet id, but total measured traffic still dropped by
about 21.9%. ACK processing time also improved.

## Experiment 2: Compact Network Entity IDs

Rationale: after ACK compression, the largest remaining fixed wire overhead in
deltas was the 64-bit server entity id on every update record. This experiment
assigns each replicated slot a monotonic 32-bit network id. Delta and destroy
records carry only that id; full upserts carry both the compact id and the
original 64-bit server entity so client-facing `server_entity` lookup semantics
remain intact.

Result:

- `wall=13.438199`
- `server_replication=6.772967`
- `client_receive=5.646756`
- `ack_processing=0.740587`
- `server_to_clients.bytes=217098318`
- `clients_to_server.bytes=706896`
- `clients_to_server.ack_records=174033`
- `clients_to_server.ack_record_bits=5569056`
- `server_to_clients.delta_upsert_bits=1505738096`
- `server_to_clients.destroy_record_bits=1531497`
- `server_to_clients.full_upsert_bits=212611680`

Conclusion: accepted. Downstream traffic dropped from the original `243959159`
bytes to `217098318` bytes despite full upserts growing by 32 bits each. Delta
upserts and destroys dominate this workload, so the compact id wins overall.
Combined with packet ACKs, total measured traffic dropped by about 30.7% versus
baseline.

## Experiment 3: Compact Full-Upsert Slot Indices

Rationale: full upserts still wrote each synced slot index as 16 bits even
though the stress archetype has five sync slots. This experiment encodes
full-upsert slot indices with the minimum archetype-local bit width,
`bits_for_range(component_count + 1)`, while leaving delta changed masks
unchanged.

Result:

- `wall=14.310530`
- `server_replication=7.138747`
- `client_receive=6.085403`
- `ack_processing=0.816023`
- `server_to_clients.bytes=213549534`
- `clients_to_server.bytes=694421`
- `clients_to_server.ack_records=170915`
- `clients_to_server.ack_record_bits=5469280`
- `server_to_clients.full_upsert_bits=184313674`
- `server_to_clients.delta_upsert_bits=1505916863`
- `server_to_clients.destroy_record_bits=1532916`

Conclusion: accepted. Full-upsert bits dropped from `212611680` to
`184313674`, and server-to-client bytes dropped from `217098318` to
`213549534`. The gain is smaller than the ACK and entity-id changes because
deltas dominate this stress workload, but the encoding is strictly more compact
for full records and keeps the same component ordering semantics.
