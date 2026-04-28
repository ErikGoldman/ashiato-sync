# Optimization Experiments

Benchmark scenario unless noted:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

The `build-bench` configuration uses `RelWithDebInfo`,
`KAGE_SYNC_BUILD_BENCHMARKS=ON`, and:

```sh
--duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5
--poison-min 1 --poison-max 8 --health-min 20 --health-max 80
--latency-ms 50 --jitter-ms 25 --loss-percent 1
--client-mode buffered-interpolation --interpolation-buffer-frames 2
--time-dilation-min 0.95 --time-dilation-max 1.05
--time-dilation-gain 0.05 --report text
```

## Baseline

Measured before the experiments in this file:

- `wall=38.018327`
- `server_sim=0.059811`
- `server_replication=17.867388`
- `client_receive=17.123224`
- `ack_processing=2.246250`

## Experiment 1: Store Snapshot Component Index

Rationale: `ReplicationServer::write_entity_record` was doing a linear search
through the archetype components for every component in every serialized entity
record. The component index is already known while quantizing a snapshot, so the
snapshot can retain it and write the same wire format without repeating that
search.

Result:

- `wall=36.936247`
- `server_sim=0.056318`
- `server_replication=17.623020`
- `client_receive=16.381917`
- `ack_processing=2.175992`

Conclusion: accepted. The change improved end-to-end wall time by about 2.8%
and reduced the measured server replication phase without changing packet
counts or bytes.

## Experiment 2: Lazy Same-Frame Snapshot Cache

Rationale: bandwidth-limited ticks should not quantize every entity up front,
but once an entity reaches serialization for one client, other clients in the
same frame should be able to reuse that quantized snapshot. This experiment adds
a direct per-slot cache for all-audience archetypes keyed by the current frame,
avoiding the retained-snapshot scan that made the earlier snapshot-reuse attempt
unattractive.

Result:

- Run 1: `wall=38.148214`, `server_replication=17.892449`,
  `client_receive=17.131745`, `ack_processing=2.326221`
- Run 2: `wall=37.979759`, `server_replication=17.868474`,
  `client_receive=17.014164`, `ack_processing=2.331807`

Conclusion: rejected. Even with a direct per-slot cache instead of a retained
snapshot scan, the result was worse than the accepted component-index change and
roughly back at the original baseline. The likely issue is that the current
snapshot path was already reusing retained snapshots after paying quantization,
while the added slot metadata and cache checks did not remove enough work in the
actual bandwidth-limited candidate pattern. A future version should only revisit
this with profiler counters that prove repeated same-slot, same-frame
quantization remains a top cost after cheaper record-writing fixes.

## Experiment 3: Patch Component Payload Length In Place

Rationale: `ReplicationServer::write_entity_record` previously serialized each
component into a temporary `BitBuffer`, wrote the temporary bit count, then
copied the temporary into the entity record. This experiment adds bounded
in-place bit overwriting to `BitBuffer`, writes a placeholder payload length,
serializes the component directly into the record, and patches the payload bit
count afterward.

Result:

- Run 1: `wall=36.823086`, `server_replication=17.165819`,
  `client_receive=16.603173`, `ack_processing=2.251198`
- Run 2: `wall=38.353138`, `server_replication=17.486119`,
  `client_receive=17.654778`, `ack_processing=2.335235`

Conclusion: rejected for now. The server replication phase improved in both
runs, which suggests removing the temporary component buffer can help that
local hot path. The end-to-end stress result was not stable, and the
confirmation run regressed wall time substantially. Do not keep this until a
less noisy benchmark series or profiler run shows that the server gain is not
being offset by instruction/cache effects elsewhere.

## Experiment 4: Update Duplicate ACKs In Place

Rationale: replacing ACK vectors with hash maps regressed because it destroyed
cache locality and added hashing overhead. This experiment keeps the compact
vector but changes duplicate ACK handling from `erase` plus `push_back` to
updating the existing record's frame in place. That preserves O(n) lookup on the
small vector while avoiding element moves and allocator-visible churn.

Result:

- `wall=37.218850`
- `server_replication=17.041773`
- `client_receive=17.016292`
- `ack_processing=2.323319`

Conclusion: rejected. Although the server phase improved, the end-to-end wall
time regressed and packet/update counts changed slightly. Keeping an older ACK
record in place changes ACK packet ordering compared with erase-and-append,
which is enough to alter loss/ACK timing and retained snapshots in this
deterministic stress setup. A future ACK redesign should preserve the existing
wire ordering semantics or explicitly benchmark a new ACK ordering policy.

## Experiment 5: Fixed-Size Direct Component Payloads

Rationale: Experiment 3 showed a consistent local server-replication
improvement but unstable wall time. The first implementation patched the
32-bit payload length with a per-bit loop. This revision keeps the same direct
serialization shape, but implements `BitBuffer::overwrite_unsigned_bits` with
fast paths for byte-aligned fields and small unaligned byte windows. It also
adds an explicit fixed serialized-size field to `SyncComponentOps`; fixed-size
components write the known payload length before serializing directly and avoid
the patch path entirely. The goal is to remove the temporary component
`BitBuffer` and copy without adding a hot length-patching cost to fixed-size
payloads.

Result:

- Run 1: `wall=34.999433`, `server_replication=16.046685`,
  `client_receive=15.937262`, `ack_processing=2.176246`
- Run 2: `wall=34.960304`, `server_replication=15.855983`,
  `client_receive=16.119773`, `ack_processing=2.150277`

Conclusion: accepted. The fixed-size path avoids the temporary component
payload buffer and avoids the patching cost for the stress components. Packet
counts and bytes stayed unchanged, while wall time improved by about 5.3% over
Experiment 1 and about 8.0% over the original baseline. Variable-size
serializers still use the in-place patch path, but this stress workload now
uses the fixed-size fast path.

## Experiment 6: Stale Duplicate ACK Records

Rationale: Experiment 4 regressed because updating duplicate ACKs in place
changed ACK ordering. The previous behavior effectively moves a refreshed ACK
to the back by erasing the old record and appending the new one. This experiment
preserves that live ordering by marking the old record stale and appending the
new record, then skipping stale records during packet drain. The goal is to
avoid erase/memmove churn while keeping the same emitted ACK order.

Result:

- `wall=35.895583`
- `server_replication=15.810462`
- `client_receive=17.090450`
- `ack_processing=2.227892`

Conclusion: rejected. Packet counts, bytes, update counts, and retained
snapshot counts matched the accepted baseline, so marking stale records did
preserve effective ACK ordering. However, wall time regressed versus Experiment
5. The server phase improved slightly, but client receive and ACK processing got
worse. The likely cost is that `drain_ack_packets` now scans stale entries and
patches packet ACK counts, while `queue_ack` still does the same linear
duplicate search. Without an index or much larger duplicate chains, avoiding the
erase/memmove is not enough to pay for the extra stale-record work.

## Experiment 7: Direct Component Deserialize Without Payload Length

Rationale: Experiment 3 and Experiment 5 showed that avoiding a temporary
component payload buffer helps, but the protocol still carried a 32-bit payload
length per component. The client immediately copied those bits into another
`BitBuffer` just so the component deserializer could consume a bounded buffer.
This experiment removes the per-component payload length from the wire format
entirely. The server writes `component_index` followed directly by the
component serializer output, and the client passes the shared packet
`BitBuffer` into each component deserializer. Fixed-size serializers are still
validated by checking the consumed bit count against `serialized_size_bits`.

Result:

- Run 1: `wall=32.424255`, `server_replication=14.474298`,
  `client_receive=15.237950`, `ack_processing=2.124191`,
  `server_to_clients.bytes=386720019`
- Run 2: `wall=32.413548`, `server_replication=14.352585`,
  `client_receive=15.275513`, `ack_processing=2.136843`,
  `server_to_clients.bytes=386720019`

Command:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: accepted. Removing the length field and direct-deserializing from
the packet improves wall time by about 7.3% over Experiment 5 and about 14.7%
over the original baseline. It also reduces server-to-client traffic from
`425722351` bytes in Experiment 5 to `386720019` bytes in this run. The cost is
that component serializers now form a stricter protocol contract: each
deserializer must consume exactly its own payload and leave the read index
positioned for the next component.

Follow-up cleanup: removed the fixed-size serializer metadata and consumed-bit
validation path after the protocol moved to direct deserialization. This keeps a
single serializer contract instead of splitting fixed and variable paths.

- Cleanup run: `wall=32.645514`, `server_replication=14.586273`,
  `client_receive=15.283563`, `ack_processing=2.178506`,
  `server_to_clients.bytes=386720019`

## Experiment 8: Heap-Based Server Candidate Priority Queue

Rationale: serialized server ticks build a candidate list for each client and
sort it by priority before packing entity records. This experiment replaces the
full `std::stable_sort` with a heap priority queue over the same candidate
vector and the same priority comparator. The goal is to avoid fully sorting
the candidate set up front while preserving the existing scheduling policy.

Result:

- `wall=33.675880`
- `server_replication=15.105663`
- `client_receive=15.736657`
- `ack_processing=2.210176`
- `server_to_clients.bytes=386720019`

Command:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: rejected. Packet counts, bytes, retained snapshots, and update
counts matched the accepted direct-deserialize baseline, so this did not change
the effective scheduling output. It did regress wall time and the server
replication phase. The likely issue is that the workload usually consumes a
large fraction of the candidate list, so heapifying and repeatedly popping
adds worse constant factors and less cache-friendly access than sorting once
and scanning the contiguous sorted vector. A viable priority-queue scheduler
probably needs to be persistent across ticks and avoid rebuilding all
candidates, not just swap `stable_sort` for a per-tick heap.

## Experiment 9: Dirty Component Delta Bitvectors

Rationale: the direct-deserialize protocol still serialized every visible
component for every delta update. This experiment uses ECS dirty bits as input
to server-owned per-slot component generations, so the server can reuse clean
component bytes from the ACKed baseline without re-quantizing or serializing
them. Delta records now carry a component bitvector mapped to the archetype;
set bits are followed by component delta payloads in archetype order, and an
all-zero bitvector still advances the entity's replicated frame for
interpolation.

Result:

- Run 1: `wall=32.017058`, `server_replication=11.400723`,
  `client_receive=17.925622`, `ack_processing=2.219748`,
  `server_to_clients.bytes=243252485`, `server_to_clients.packets=207125`
- Run 2: `wall=31.427831`, `server_replication=11.106562`,
  `client_receive=17.628005`, `ack_processing=2.161517`,
  `server_to_clients.bytes=243252485`, `server_to_clients.packets=207125`

Command:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: accepted. Server replication time improved substantially and
server-to-client traffic fell from `386720019` bytes to `243252485` bytes.
Client receive got slower because it now merges partial deltas with baseline
state, but the server and bandwidth savings more than offset that in the
end-to-end benchmark. Delta upsert counts stayed effectively unchanged, while
packet count dropped because smaller entity records pack more densely. The
server observes but does not clear gameplay dirty bits; the stress harness
clears the replicated component dirty bits after server replication to model
application-owned dirty-bit lifecycle.
