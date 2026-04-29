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

## Experiment 10: Same-Frame Quantized Snapshot Cache After Dirty Bitvectors

Rationale: Experiment 9 made ACKed baseline reuse much cheaper, but the server
still rebuilt same-frame quantized snapshots when multiple clients serialized
the same all-audience slot in one server frame. This revisits the rejected
Experiment 2 idea on top of the newer dirty-component protocol. Two variants
were tried:

- direct per-slot same-frame snapshot id cache with archetype audience checked
  on every serialization;
- the same snapshot id cache with the all-audience eligibility precomputed on
  the replicated slot when its archetype is assigned.

Result:

- Direct eligibility check run: `wall=32.632117`,
  `server_replication=11.080315`, `client_receive=18.685556`,
  `ack_processing=2.320491`, `server_to_clients.bytes=243252485`
- Precomputed eligibility run 1: `wall=30.170488`,
  `server_replication=10.074138`, `client_receive=17.413807`,
  `ack_processing=2.185484`, `server_to_clients.bytes=243252485`
- Precomputed eligibility run 2: `wall=31.055385`,
  `server_replication=10.351669`, `client_receive=17.939104`,
  `ack_processing=2.233395`, `server_to_clients.bytes=243252485`

Focused benchmark command:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ServerTickPacked(FullBudget|AckedDeltaShared|MtuLimited)|BM_ServerTickMutatingAckedDelta|BM_ServerTickOwnerAudienceMixed|BM_ServerTickArchetypeDiversity' --benchmark_min_time=0.05s
```

Stress command:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: accepted with precomputed eligibility. The direct-check variant
reduced server replication but regressed wall time, matching the earlier lesson
that cache metadata overhead can erase quantization wins. Precomputing the
cacheability bit keeps the hot-path check small enough to pay off: compared
with Experiment 9's accepted runs, server replication improved from
`11.106562-11.400723` seconds to `10.074138-10.351669` seconds, while wall time
improved from `31.427831-32.017058` seconds to `30.170488-31.055385` seconds.
Packet counts, update counts, retained snapshot counts, and
server-to-client bytes stayed unchanged, so the gain is CPU-side only.

## Experiment 11: Client Decode Allocation Variants

Rationale: client receive remained the largest measured phase after dirty
component bitvectors. Three client/server allocation variants were tried on the
stress workload:

- persistent `ReplicationClient` decode/merge/change scratch vectors instead
  of per-record local vectors;
- a direct storage apply fast path for storage-identical component traits using
  runtime ECS byte writes;
- a small-archetype delta change-mask fast path that reads component change
  bits into a local `uint64_t` instead of allocating a `vector<bool>`;
- with the explicit assumption that archetypes will never exceed 64 components,
  a client bulk `read_unsigned_bits(component_count)` variant and a server
  delta-writer mask variant were also tried.

Results:

- Persistent client scratch: `wall=31.154470`, `server_replication=10.328614`,
  `client_receive=18.061907`, `ack_processing=2.269345`
- Direct storage apply: `wall=31.987381`, `server_replication=10.580664`,
  `client_receive=18.682031`, `ack_processing=2.218531`
- 64-bit change mask run 1: `wall=30.216835`,
  `server_replication=10.388842`, `client_receive=17.196505`,
  `ack_processing=2.151588`
- 64-bit change mask run 2: `wall=31.621334`,
  `server_replication=10.822611`, `client_receive=18.009891`,
  `ack_processing=2.266083`
- 64-bit change mask run 3: `wall=30.765997`,
  `server_replication=10.513511`, `client_receive=17.547391`,
  `ack_processing=2.221906`
- Client bulk bitset read run 1: `wall=30.590865`,
  `server_replication=10.522056`, `client_receive=17.371195`,
  `ack_processing=2.184699`
- Client bulk bitset read run 2: `wall=30.070997`,
  `server_replication=10.204007`, `client_receive=17.240643`,
  `ack_processing=2.149421`
- Server delta-writer mask plus client bitset read run 1: `wall=31.142853`,
  `server_replication=10.565002`, `client_receive=17.851370`,
  `ack_processing=2.249158`
- Server delta-writer mask plus client bitset read run 2: `wall=31.488401`,
  `server_replication=10.742865`, `client_receive=18.034050`,
  `ack_processing=2.223578`

Baseline immediately before these experiments:

- `wall=30.966659`, `server_replication=10.377538`,
  `client_receive=17.888676`, `ack_processing=2.222215`

Command:

```sh
cmake --build build-bench --target run_kage_sync_ball_stress
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: keep only the client bulk bitset read. The persistent scratch
variant did not help, likely because the retained vector capacity increased
client working-set pressure and the remaining copies still dominated. The
direct storage apply path was worse; the dynamic ECS byte-add route appears
slower than the typed `registry.add<T>` path for this workload. The first
client mask variant removed the `vector<bool>` allocation, and the follow-up
bulk read shrank the hot path further now that archetypes are assumed to fit in
64 bits. Packet/update counts stayed identical. The server-side mask variant
regressed stress timing; the original changed-pointer vector plus existing
searches is apparently cheaper than the added mask/order checks in this
workload.

## Experiment 12: Component-Oriented Iteration Attempts

Rationale: after the client bitset change, two component-oriented ideas were
tested:

- store client baselines with component indexes and use fixed 64-entry stack
  lookup tables during delta merge instead of repeated linear searches;
- during server snapshot construction, copy clean components from the ACKed
  baseline before doing `registry.get`, and separately try a per-slot component
  cache filled during dirty component iteration.

Results:

- Client indexed-baseline merge: `wall=33.236714`,
  `server_replication=11.102449`, `client_receive=19.223582`,
  `ack_processing=2.345817`, `rss_peak_bytes=1057853440`
- Server dirty-quantized component cache run 1: `wall=30.892461`,
  `server_replication=10.424174`, `client_receive=17.724703`,
  `ack_processing=2.237924`
- Server dirty-quantized component cache run 2: `wall=30.934188`,
  `server_replication=10.420661`, `client_receive=17.781287`,
  `ack_processing=2.254061`
- Server copy-clean-before-ECS-get run 1: `wall=30.707352`,
  `server_replication=10.489335`, `client_receive=17.544651`,
  `ack_processing=2.194878`
- Server copy-clean-before-ECS-get run 2: `wall=30.682914`,
  `server_replication=10.429197`, `client_receive=17.547564`,
  `ack_processing=2.237003`
- Client bulk bitset read with the `<=64 components` guard removed:
  `wall=29.702147`, `server_replication=10.215634`,
  `client_receive=16.853907`, `ack_processing=2.157355`

Focused server benchmark for the copy-clean-before-ECS-get variant:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ServerTickPackedAckedDeltaShared|BM_ServerTickMutatingAckedDelta' --benchmark_min_time=0.05s
```

The focused server results regressed versus the earlier benchmark run in this
file, so the server-side variants were rejected.

Stress command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --report text
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: rejected the client indexed-baseline merge because the stored
component index inflated every retained baseline and buffered frame enough to
hurt memory and client receive substantially. Rejected the server component
cache because the retained per-slot cache and extra work in dirty capture did
not improve server replication. Rejected copy-clean-before-ECS-get because it
regressed focused server benchmarks. Kept only the branch removal in the client
bulk bitset read path under the project assumption that archetypes never exceed
64 components.

## Experiment 13: Inline Bytes and Archetype-Indexed Ops

Rationale: the previous component-oriented attempts showed that adding indexes
to every retained component record inflated memory too much. This experiment
instead changes the byte payload itself and uses existing archetype order:

- replace heap-first `std::vector<uint8_t>` quantized payloads with
  `QuantizedBytes`, which stores payloads up to 16 bytes inline and lazily
  allocates overflow storage for larger payloads;
- use archetype-order forward scans during client delta merge instead of
  repeated `find_if` calls, without storing a component index per baseline;
- store a copy of component ops on each archetype so indexed server/client hot
  paths avoid `unordered_map` lookups.

Results:

- Inline bytes with embedded overflow vector run 1: `wall=24.970765`,
  `server_replication=9.711238`, `client_receive=12.783843`,
  `ack_processing=2.063182`, `rss_peak_bytes=895598592`
- Inline bytes with embedded overflow vector run 2: `wall=24.639514`,
  `server_replication=9.588017`, `client_receive=12.602594`,
  `ack_processing=2.010520`, `rss_peak_bytes=895639552`
- Add archetype-order client merge run 1: `wall=24.256349`,
  `server_replication=9.531549`, `client_receive=12.346919`,
  `ack_processing=1.977476`
- Add archetype-order client merge run 2: `wall=24.655805`,
  `server_replication=9.689080`, `client_receive=12.533034`,
  `ack_processing=2.000763`
- Add precomputed archetype component ops run 1: `wall=24.721719`,
  `server_replication=9.644244`, `client_receive=12.650505`,
  `ack_processing=2.027500`
- Add precomputed archetype component ops run 2: `wall=24.394010`,
  `server_replication=9.528032`, `client_receive=12.428334`,
  `ack_processing=2.024985`
- Map lookup isolation run on the same inline/order code: `wall=24.085248`,
  `server_replication=9.964842`, `client_receive=11.705891`,
  `ack_processing=2.027171`
- Lazy overflow pointer run 1: `wall=23.789934`,
  `server_replication=9.868873`, `client_receive=11.540013`,
  `ack_processing=1.990558`, `rss_peak_bytes=704483328`
- Lazy overflow pointer run 2: `wall=23.291535`,
  `server_replication=9.554155`, `client_receive=11.408701`,
  `ack_processing=1.945955`, `rss_peak_bytes=704274432`
- Final kept state run: `wall=24.017574`,
  `server_replication=9.849401`, `client_receive=11.768045`,
  `ack_processing=2.002623`, `rss_peak_bytes=704196608`

Command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --report text
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: accepted. Inline/lazy quantized storage is the dominant win: the
stress components all fit in 16 bytes, so retained snapshots, client baseline
history, buffered frames, and display samples avoid millions of tiny vector
allocations and reduce RSS by about 263 MB versus the previous accepted state.
Archetype-order client merge gives a small additional CPU win without storing
per-component indexes. Precomputed archetype component ops are mixed in stress,
but they remove hash lookups from indexed hot paths and remain in the final
state; the map-isolation run had better client time but worse server
replication. Packet/update counts stayed identical across all accepted runs.

## Experiment 14: Fixed-Size Archetype Frame Storage

Rationale: `SyncComponentTraits<T>::Quantized` is already required to be
trivially copyable, so each component's retained quantized state has a known
fixed size. Instead of storing one byte container per retained component, this
experiment precomputes per-archetype byte offsets and stores retained client
and server state as one byte blob plus a 64-bit component presence mask.

Implementation notes:

- `SyncArchetype` now stores `component_offsets` and
  `total_quantized_bytes`, validated at archetype definition time.
- Client `EntityState` baselines, baseline history, buffered frames, and
  applied component tracking use `QuantizedFrameData`.
- Server `QuantizedSnapshot` uses `QuantizedFrameData` plus a parallel
  component dirty-generation vector.
- Component-facing callbacks/display paths still materialize temporary
  `ReplicatedComponentUpdate` values where needed.
- Wire format is unchanged.

Results:

- Client-only fixed frame run: `wall=22.157833`,
  `server_replication=9.970254`, `client_receive=10.048042`,
  `ack_processing=1.755859`, `rss_peak_bytes=352415744`,
  `server_retained_snapshot_bytes=101944`
- Full client/server fixed frame run 1: `wall=22.409071`,
  `server_replication=9.958881`, `client_receive=10.288488`,
  `ack_processing=1.775840`, `rss_peak_bytes=350326784`,
  `server_retained_snapshot_bytes=107456`
- Full client/server fixed frame run 2: `wall=21.216457`,
  `server_replication=9.412056`, `client_receive=9.773369`,
  `ack_processing=1.678802`, `rss_peak_bytes=350318592`,
  `server_retained_snapshot_bytes=107456`

Focused benchmark:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ClientReceiveBufferedInterpolation|BM_ServerTickPackedAckedDeltaShared|BM_ServerTickMutatingAckedDelta' --benchmark_min_time=0.05s
```

Stress command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --report text
```

Artifact: `build-bench/kage_sync_ball_stress`

Conclusion: accepted. Compared with Experiment 13's lazy-overflow retained
component records, fixed archetype frame storage cuts RSS by roughly another
354 MB and reduces client receive from about `11.4-11.8` seconds to
`9.8-10.3` seconds. Server retained snapshot bytes rise slightly because the
fixed blob stores the full archetype byte span for each snapshot, but the
combined process RSS and wall time improve substantially. Packet/update counts
stayed identical.

## Experiment 15: Post-Profile Hot Path Batch

Rationale: a gprof run of the documented ball stress scenario showed the
largest remaining costs in per-component byte scratch during
serialize/deserialize, buffered client receive work that is unused when no
entity mode selector is installed, ACK duplicate handling, server candidate
sorting, and unaligned `BitBuffer` integer reads/writes. This experiment tried
the five targeted changes from that profile in sequence and ran the stress
scenario after each step:

- direct byte-span component ops for fixed archetype frame storage;
- buffered interpolation receive fast path when no entity mode selector is
  installed;
- indexed ACK coalescing with append-order preservation;
- merge ordered updates with sorted pending destroys instead of sorting all
  server candidates;
- unaligned `BitBuffer::push_unsigned_bits` and `read_unsigned_bits` fast
  paths.

Baseline immediately before these changes:

- `wall=21.268996`, `server_replication=9.462134`,
  `client_receive=9.784310`, `ack_processing=1.667472`

Stress results after each step:

- Direct byte-span ops: `wall=21.851678`,
  `server_replication=9.485185`, `client_receive=10.288461`,
  `ack_processing=1.724624`
- Add buffered receive fast path: `wall=21.156217`,
  `server_replication=9.479551`, `client_receive=9.650104`,
  `ack_processing=1.689691`
- Add indexed ACK coalescing: `wall=21.956637`,
  `server_replication=10.023671`, `client_receive=9.363564`,
  `ack_processing=2.222341`
- Add candidate merge scheduler: `wall=20.507879`,
  `server_replication=9.253099`, `client_receive=8.720641`,
  `ack_processing=2.197905`
- Add unaligned `BitBuffer` fast paths: `wall=17.570524`,
  `server_replication=8.440212`, `client_receive=6.979565`,
  `ack_processing=1.826202`
- Replace ACK hash index with direct entity-index table and fix scheduler
  same-tick destroy collection in the final state: `wall=14.560662`,
  `server_replication=7.585361`, `client_receive=5.547014`,
  `ack_processing=1.145871`

Stress command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --report text
```

Stress artifact: `build-bench/kage_sync_ball_stress`

Focused benchmark command:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ClientReceiveBufferedInterpolation|BM_ClientDrainDuplicateHeavyAckPackets|BM_ServerTickStressScheduler|BM_ServerTickPackedAckedDeltaShared|BM_ServerTickMutatingAckedDelta|BM_BitBufferUnalignedReadUnsigned' --benchmark_min_time=0.05s
```

Focused artifact: `build-bench/kage_sync_benchmark`

Profile command:

```sh
cmake --build build-bench-gprof --target run_kage_sync_ball_stress
```

Profile artifact: `/tmp/kage_sync_ball_stress_gprof.txt`

Profiled final-state stress result: `wall=18.123062`,
`server_replication=8.897216`, `client_receive=7.355429`,
`ack_processing=1.316898`

Focused results from the final state:

- `BM_ClientReceiveBufferedInterpolation/4096/16`: `7991084 ns`
- `BM_ClientDrainDuplicateHeavyAckPackets/4096/64`: `4572208 ns`
- `BM_ServerTickStressScheduler/4096/4`: `6071280 ns`
- `BM_BitBufferUnalignedReadUnsigned/16384`: `41403 ns`
- `BM_ServerTickPackedAckedDeltaShared/16384/8`: `15938889 ns`
- `BM_ServerTickMutatingAckedDelta/16384/8`: `77390329 ns`

Conclusion: keep the buffered receive fast path, direct entity-index ACK
coalescing, and unaligned `BitBuffer` fast paths. The direct byte-span
component ops regressed by themselves, but they enabled the buffered receive
fast path and leave a useful hook for future component-direct work. The first
ACK index attempt using an `unordered_map` regressed; changing the index to a
direct table keyed by `ecs::Registry::entity_index` made the final cumulative
state much faster. The candidate merge scheduler improved wall time, but it
changed packet/update counts slightly, so it should be treated as a scheduling
policy change rather than a pure CPU optimization. The final cumulative state
cuts wall time by about 31.5% versus the immediate baseline.

## Experiment 16: Server-Side Serialized Packet Parallelism

Rationale: This experiment only parallelizes server-side replication fanout across server
client states.

The first attempt ran each server client on a worker thread and guarded
snapshot creation/retention/release with one shared mutex. It preserved packet
counts, but lock contention around `serialize_entity` dominated:

- 1 worker: `wall=14.882651`, `server_replication=7.468835`,
  `client_receive=5.852739`, `ack_processing=1.262971`
- 2 workers: `wall=16.938433`, `server_replication=9.289803`,
  `client_receive=6.188095`, `ack_processing=1.186295`
- 4 workers: `wall=22.046163`, `server_replication=14.195213`,
  `client_receive=6.331154`, `ack_processing=1.243985`
- 8 workers: `wall=22.636699`, `server_replication=14.470752`,
  `client_receive=6.571749`, `ack_processing=1.300797`

The accepted version uses two phases. It prepares candidate order and
quantized snapshots serially, retaining each prepared snapshot. Workers then
write entity records, pack packets, and mutate only their own server-side
client state. Snapshot releases and transport callbacks are committed serially
after workers join.

Stress results:

- 1 worker: `wall=15.296896`, `server_replication=7.781807`,
  `client_receive=5.967734`, `ack_processing=1.265170`
- 2 workers: `wall=12.115623`, `server_replication=4.674807`,
  `client_receive=5.984037`, `ack_processing=1.183565`
- 4 workers: `wall=10.549999`, `server_replication=3.122620`,
  `client_receive=5.997861`, `ack_processing=1.154988`
- 8 workers: `wall=10.706439`, `server_replication=3.156206`,
  `client_receive=6.110277`, `ack_processing=1.165487`

All stress runs produced identical packet/update counts:
`server_to_clients packets=207106 bytes=243226067 full_upserts=489921
delta_upserts=7332075 destroys=45716` and
`clients_to_server packets=61172 bytes=70817714`.

Stress command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --server-worker-threads N --report text
```

Stress artifact: `build-bench/kage_sync_ball_stress`

Focused benchmark command:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ServerTickStressScheduler' --benchmark_min_time=0.05s
```

Focused artifact: `build-bench/kage_sync_benchmark`

Focused results:

- `BM_ServerTickStressScheduler/4096/4/1`: `7099466 ns`
- `BM_ServerTickStressScheduler/4096/4/2`: `6174418 ns`
- `BM_ServerTickStressScheduler/4096/4/4`: `4663740 ns`

Profile command:

```sh
cmake --build build-bench-gprof --target run_kage_sync_ball_stress
```

Profile artifact: `/tmp/kage_sync_ball_stress_gprof.txt`

Profiled 4-worker stress result: `wall=15.492355`,
`server_replication=5.647791`, `client_receive=7.856186`,
`ack_processing=1.425256`. The flat profile still shows the parallel server
packing worker as the largest bucket, followed by client `apply_upsert`,
component serialization, client `apply_frame`, ACK processing, and serial
snapshot preparation.

Conclusion: keep the two-phase server-side parallel serialization path as an
opt-in `ReplicationServerOptions::serialized_worker_threads`. The useful limit
is the number of server-side client states; in this 4-client stress case, 4
workers is best and 8 workers adds no value. Do not parallelize simulated
client receive work for this benchmark, because that does not correspond to a
single real client's performance.

## Experiment 17: Serial Client State Locality

Rationale: improve real single-client performance without parallelizing
simulated clients. The client previously stored replicated entity state in an
`unordered_map` keyed by full server entity value and scanned that map for
buffered apply/display and snap-error blending. This experiment changed the
client to direct sparse storage keyed by `ecs::Registry::entity_index`, added
dense active/buffered/snap-error index lists, and changed per-entity baseline
history to ring-style replacement.

Focused baseline:

- `BM_ClientReceiveSnap/4096/16`: `7214368 ns`
- `BM_ClientReceiveBufferedInterpolation/4096/16`: `9138283 ns`
- `BM_ClientReceiveMixedEntityModes/4096/16`: `8005884 ns`
- `BM_ClientApplyBufferedInterpolation/4096/16`: `3750218 ns`
- `BM_ClientSampleDisplayInterpolation/4096/16`: `4615188 ns`
- `BM_ClientDrainDuplicateHeavyAckPackets/4096/64`: `5393868 ns`
- `BM_ClientTickBufferedAutoInterpolation/4096/16`: `28556498 ns`

Focused final:

- `BM_ClientReceiveSnap/4096/16`: `7538320 ns`
- `BM_ClientReceiveBufferedInterpolation/4096/16`: `7986956 ns`
- `BM_ClientReceiveMixedEntityModes/4096/16`: `8465797 ns`
- `BM_ClientApplyBufferedInterpolation/4096/16`: `3097993 ns`
- `BM_ClientSampleDisplayInterpolation/4096/16`: `3837330 ns`
- `BM_ClientDrainDuplicateHeavyAckPackets/4096/64`: `5020239 ns`
- `BM_ClientTickBufferedAutoInterpolation/4096/16`: `16780657 ns`

Focused command:

```sh
build-bench/kage_sync_benchmark --benchmark_filter='BM_ClientReceiveSnap|BM_ClientReceiveBufferedInterpolation|BM_ClientReceiveMixedEntityModes|BM_ClientApplyBufferedInterpolation|BM_ClientSampleDisplayInterpolation|BM_ClientTickBufferedAutoInterpolation|BM_ClientDrainDuplicateHeavyAckPackets' --benchmark_min_time=0.05s
```

Focused artifact: `build-bench/kage_sync_benchmark`

Stress result:

- `wall=13.832802`, `server_replication=7.279544`,
  `client_receive=5.149021`, `ack_processing=1.175458`,
  `rss_peak_bytes=157884416`
- Packet/update counts changed slightly because stale lower-generation packets
  for reused entity indices are now rejected instead of creating a second
  client state for the old generation:
  `server_to_clients packets=207118 bytes=243233732 full_upserts=490132
  delta_upserts=7331864 destroys=45735`;
  `clients_to_server packets=61211 bytes=70858348`.

Stress command:

```sh
build-bench/kage_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --report text
```

Stress artifact: `build-bench/kage_sync_ball_stress`

Profile command:

```sh
cmake --build build-bench-gprof --target run_kage_sync_ball_stress
```

Profile artifact: `/tmp/kage_sync_ball_stress_gprof.txt`

Profiled final-state stress result: `wall=19.174335`,
`server_replication=9.528776`, `client_receive=7.536991`,
`ack_processing=1.564016`. The flat profile's largest bucket is still default
single-worker server serialization; the remaining client-side buckets are
`apply_upsert`, `remember_baseline`, and `apply_frame`.

Conclusion: keep the direct indexed client state, dense buffered/snap-error
iteration lists, and ring baseline history. Large buffered client paths improve
substantially, especially full receive+tick. Large snap and mixed-mode receive
are neutral to slightly slower, but the stress run improves wall time, client
receive time, ACK processing, and RSS. The generation-reuse behavior is also
stricter and avoids stale packets mutating the wrong entity slot.
