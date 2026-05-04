# Optimization Experiments

## Header Encapsulation Refactor: Move Private State Out of Public Headers

Rationale: `ReplicationClient` and `ReplicationServer` exposed large private
state records in public headers. A pimpl would hide more of the object layout,
but would also add allocation and pointer indirection to a high-performance
library. This refactor keeps state stored inline and moves private record
definitions into internal source headers.

Result:

- Object sizes unchanged on this toolchain:
  `ReplicationClient=1496`, `ReplicationServer=648`,
  `ReplicationClientOptions=256`, `ReplicationServerOptions=216`
- Public client/server preprocessed line count: `77104` before, `76928` after
- Focused benchmark pass 1:
  `BM_ClientReceiveSnap/1024/16=2035138 ns`,
  `BM_ClientPredictTickQuantize/1024/16=839062 ns`,
  `BM_ServerTickSerializedFullBudget/16384/8=39017415 ns`,
  `BM_ServerProcessAckPacket/1024=151512 ns`
- Focused benchmark pass 2:
  `BM_ClientReceiveSnap/1024/16=2291686 ns`,
  `BM_ClientPredictTickQuantize/1024/16=984840 ns`,
  `BM_ServerTickSerializedFullBudget/16384/8=42396840 ns`,
  `BM_ServerProcessAckPacket/1024=167315 ns`

Command:

```sh
./build-bench/kage_sync_benchmark --benchmark_filter='BM_(ClientReceiveSnap/1024/16|ClientPredictTickQuantize/1024/16|ServerTickSerializedFullBudget/16384/8|ServerProcessAckPacket/1024)$' --benchmark_min_time=0.1s
```

Artifact: `build-bench/kage_sync_benchmark`

Conclusion: accepted. Runtime remains in the same noise band as the pre-refactor
focused baseline while avoiding pimpl's allocation and indirection costs. The
main benefit is API hygiene: large client/server state records are no longer
defined in public headers, though full ABI hiding would still require pimpl.

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

## Experiment 1: Store Quantized Frame Component Index

Rationale: `ReplicationServer::write_entity_record` was doing a linear search
through the archetype components for every component in every serialized entity
record. The component index is already known while quantizing a frame, so the
quantized frame can retain it and write the same wire format without repeating that
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

## Experiment 2: Lazy Same-Frame Quantized Frame Cache

Rationale: bandwidth-limited ticks should not quantize every entity up front,
but once an entity reaches serialization for one client, other clients in the
same frame should be able to reuse that quantized frame. This experiment adds
a direct per-slot cache for all-audience archetypes keyed by the current frame,
avoiding the retained quantized-frame scan that made the earlier quantized-frame reuse attempt
unattractive.

Result:

- Run 1: `wall=38.148214`, `server_replication=17.892449`,
  `client_receive=17.131745`, `ack_processing=2.326221`
- Run 2: `wall=37.979759`, `server_replication=17.868474`,
  `client_receive=17.014164`, `ack_processing=2.331807`

Conclusion: rejected. Even with a direct per-slot cache instead of a retained
quantized frame scan, the result was worse than the accepted component-index change and
roughly back at the original baseline. The likely issue is that the current
quantized frame path was already reusing retained quantized frames after paying quantization,
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
which is enough to alter loss/ACK timing and retained quantized frames in this
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
quantized frame counts matched the accepted baseline, so marking stale records did
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

Conclusion: rejected. Packet counts, bytes, retained quantized frames, and update
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

## Experiment 10: Same-Frame Quantized Frame Cache After Dirty Bitvectors

Rationale: Experiment 9 made ACKed baseline reuse much cheaper, but the server
still rebuilt same-frame quantized frames when multiple clients serialized
the same all-audience slot in one server frame. This revisits the rejected
Experiment 2 idea on top of the newer dirty-component protocol. Two variants
were tried:

- direct per-slot same-frame quantized-frame id cache with archetype audience checked
  on every serialization;
- the same quantized-frame id cache with the all-audience eligibility precomputed on
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
Packet counts, update counts, retained quantized-frame counts, and
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
- during server quantized-frame construction, copy clean components from the ACKed
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
stress components all fit in 16 bytes, so retained quantized frames, client baseline
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
- Server `QuantizedFrame` uses `QuantizedFrameData` plus a parallel
  component dirty-generation vector.
- Component-facing callbacks/display paths still materialize temporary
  `ReplicatedComponentUpdate` values where needed.
- Wire format is unchanged.

Results:

- Client-only fixed frame run: `wall=22.157833`,
  `server_replication=9.970254`, `client_receive=10.048042`,
  `ack_processing=1.755859`, `rss_peak_bytes=352415744`,
  `server_retained_quantized_frame_bytes=101944`
- Full client/server fixed frame run 1: `wall=22.409071`,
  `server_replication=9.958881`, `client_receive=10.288488`,
  `ack_processing=1.775840`, `rss_peak_bytes=350326784`,
  `server_retained_quantized_frame_bytes=107456`
- Full client/server fixed frame run 2: `wall=21.216457`,
  `server_replication=9.412056`, `client_receive=9.773369`,
  `ack_processing=1.678802`, `rss_peak_bytes=350318592`,
  `server_retained_quantized_frame_bytes=107456`

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
`9.8-10.3` seconds. Server retained quantized-frame bytes rise slightly because the
fixed blob stores the full archetype byte span for each quantized frame, but the
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
quantized frame creation/retention/release with one shared mutex. It preserved packet
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
quantized frames serially, retaining each prepared quantized frame. Workers then
write entity records, pack packets, and mutate only their own server-side
client state. Quantized-frame releases and transport callbacks are committed serially
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
quantized frame preparation.

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

## Experiment 18: Prediction Stress Gprof Profile

Rationale: profile the prediction-specific stress benchmark with configurable
mispredictions and latency. This run uses the deliberately harsh `all` rollback
policy so that frequent prediction failures exercise rollback, resimulation,
current-frame requantization, and error blending.

Profile build:

```sh
cmake -S . -B build-bench-gprof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKAGE_SYNC_BUILD_BENCHMARKS=ON -DKAGE_SYNC_ENABLE_GPROF=ON -DBUILD_TESTING=OFF
cmake --build build-bench-gprof --target kage_sync_prediction_stress -j2
```

Profile command:

```sh
./kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
```

Profile artifact: `/tmp/kage_sync_prediction_stress_gprof.txt`

Profiled result:

- `wall=9.326060`
- `server_simulation=0.086520`
- `server_replication=4.639693`
- `client_receive=1.721267`
- `client_tick=2.652223`
- `ack_processing=0.225947`
- `server_packets=73279`, `server_bytes=85935455`
- `misprediction_events=184255`, `predicted_entities=2048`

Normal `RelWithDebInfo` comparison, same parameters:

- `all`: `wall=6.136722`, `client_tick=0.993263`
- `only-affected`: `wall=5.576276`, `client_tick=0.605114`

Top profile buckets:

- `ReplicationServer::tick_serialized`: 46.97% self, 1800 calls
- `PredPosition::serialize_bytes`: 4.36%, 3,686,400 calls
- `ReplicationServer::find_or_create_quantized_frame`: 4.17%, 3,686,400 calls
- `ReplicationClient::remember_baseline`: 3.60%, 3,686,400 calls
- `PredVelocity::serialize_bytes`: 3.22%, 3,661,815 calls
- `ReplicationClient::blend_resim_errors`: 2.46%, 1799 calls
- `mutable_frame_component_data`: 2.27%, 57,414,566 calls
- `frame_component_bytes`: 2.08%, 42,739,554 calls
- `ReplicationClient::apply_upsert`: 2.08%, 3,686,400 calls
- `all_zero`: 1.89%, 88,477,706 calls
- `ReplicationClient::compare_predicted_frame`: 0.95%, 3,684,352 calls
- `ReplicationClient::resimulate_all_predicted`: 0.95%, 1799 calls

Conclusion: the dominant cost in this benchmark is still full server
replication volume: every tick changes position for all 2048 predicted
entities, so the server serializes millions of entity records and about 86 MB
of update data. On the client, prediction-specific costs are mostly component
payload copying and repeated whole-population work after rollbacks. The first
follow-up optimizations should be direct raw-byte component comparison/error
hooks for prediction, O(1) baseline-history replacement keyed by frame, and
resim error blending over only the resimulated entity set instead of scanning
all predicted entities every rollback.

## Experiment 19: Raw-Byte Prediction Compare and Error Hooks

Rationale: `compare_predicted_frame` and prediction resim error blending were
calling `frame_component_bytes`, which copies fixed-size component payloads into
`QuantizedBytes` temporaries before invoking trait functions. This experiment
adds raw-byte `should_roll_back_bytes` and `compute_error_bytes` hooks to
`SyncComponentOps` and uses `frame_component_data` in the prediction path.

Baseline command:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
```

Baseline result:

- `wall=5.700782`
- `server_replication=3.628783`
- `client_receive=0.996992`
- `client_tick=0.931530`
- `ack_processing=0.105458`

Result:

- `wall=5.777797`
- `server_replication=3.776654`
- `client_receive=0.905312`
- `client_tick=0.940827`
- `ack_processing=0.115918`

Conclusion: mixed but kept. The receive-side cost improved by about 9.2%, which
matches the intended removal of component-byte copies during authoritative frame
comparison. End-to-end wall time and client tick were slightly worse in this
single run, with server replication noise still dominating the wall result. This
also gives prediction and interpolation a consistent raw-byte component API.

## Experiment 20: O(1) Baseline History Ring

Rationale: `remember_baseline` scanned up to 64 historical frames for every
received entity update before replacing or inserting a baseline. Prediction
traffic calls this millions of times in the stress run. This experiment changes
baseline history to a fixed 64-slot ring keyed directly by `frame & 63`, with a
valid bit per slot.

Command:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
```

Result:

- `wall=5.139806`
- `server_replication=3.501682`
- `client_receive=0.591888`
- `client_tick=0.907018`
- `ack_processing=0.101236`

Conclusion: accepted. Compared with Experiment 19, wall time improved by about
11.0% and client receive time improved by about 34.6%. This removes the
per-update history scan that was visible in the profile.

## Experiment 21: Blend Only Resimulated Prediction Entities

Rationale: after rollback/resim, error blending scanned every predicted entity
even when the rollback policy was `only-affected`. This experiment records the
entity indices that will be resimulated, captures original current-frame
predictions only for those indices, and blends only that set.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=5.306687`, `server_replication=3.628128`,
  `client_receive=0.597561`, `client_tick=0.934218`,
  `ack_processing=0.105500`
- `only-affected`: `wall=4.264001`, `server_replication=3.302540`,
  `client_receive=0.574346`, `client_tick=0.258931`,
  `ack_processing=0.090566`

Final confirmation after reverting Experiment 22:

- `all`: `wall=5.174080`, `server_replication=3.553726`,
  `client_receive=0.603236`, `client_tick=0.876106`,
  `ack_processing=0.102139`
- `only-affected`: `wall=4.757985`, `server_replication=3.656777`,
  `client_receive=0.664567`, `client_tick=0.293342`,
  `ack_processing=0.100373`

Conclusion: accepted. The `all` policy legitimately resimulates the full
predicted set, so this change is mostly neutral there. For `only-affected`, the
client tick cost drops substantially versus the earlier `only-affected`
comparison from Experiment 18 (`client_tick=0.605114`).

## Experiment 22: Error Compute Returns Non-Zero State

Rationale: `blend_resim_errors` computes an error payload and then calls
`all_zero` to decide whether to store it. This experiment added a
`compute_error_bytes_nonzero` hook that computes the error and reports whether
the typed error equals zero in the same trait wrapper.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=5.395147`, `server_replication=3.738402`,
  `client_receive=0.632616`, `client_tick=0.880470`,
  `ack_processing=0.104710`
- `only-affected`: `wall=4.734596`, `server_replication=3.661067`,
  `client_receive=0.655116`, `client_tick=0.281926`,
  `ack_processing=0.097310`
- `only-affected` confirmation: `wall=4.817131`,
  `server_replication=3.746629`, `client_receive=0.649715`,
  `client_tick=0.279296`, `ack_processing=0.100948`

Conclusion: rejected and reverted. The extra hook did not produce a stable
win, and the `only-affected` runs were slower than the best Experiment 21 run.
The likely reason is that replacing `all_zero` with a typed zero object plus
`memcmp` does not reduce enough work for these small error payloads, while it
adds another branch and function pointer on the hot path.

## Experiment 23: Prediction Stress Reprofile After Accepted Optimizations

Rationale: reprofile prediction stress after keeping raw-byte prediction hooks,
O(1) baseline history, and resim-error blending over the resimulated entity
set. The goal is to confirm which hot spots remain and separate `all` rollback
behavior from `only-affected` behavior.

Profile build:

```sh
cmake --build build-bench-gprof --target kage_sync_prediction_stress -j2
```

Profile commands:

```sh
./kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
./kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Profile artifacts:

- `/tmp/kage_sync_prediction_stress_all_after_opt_gprof.txt`
- `/tmp/kage_sync_prediction_stress_only_affected_after_opt_gprof.txt`

Profiled `all` result:

- `wall=8.611981`
- `server_simulation=0.082899`
- `server_replication=4.341508`
- `client_receive=1.419152`
- `client_tick=2.555149`
- `ack_processing=0.212865`

Profiled `only-affected` result:

- `wall=6.901837`
- `server_simulation=0.081641`
- `server_replication=4.334525`
- `client_receive=1.478702`
- `client_tick=0.801887`
- `ack_processing=0.204685`

Top `all` buckets:

- `ReplicationServer::tick_serialized`: 49.37%, 1800 calls
- `ReplicationServer::find_or_create_quantized_frame`: 5.27%, 3,686,400 calls
- `PredPosition::serialize_bytes`: 4.01%, 3,686,400 calls
- `frame_component_data`: 3.16%, 71,395,637 calls
- `PredVelocity::serialize_bytes`: 2.74%, 3,661,815 calls
- `ReplicationClient::blend_resim_errors`: 2.32%, 1799 calls
- `all_zero`: 2.11%, 92,164,106 calls
- `ReplicationClient::quantize_predicted_entity`: 2.11%, 7,372,800 calls
- `init_frame_data`: 1.69%, 14,745,600 calls
- `ReplicationClient::compare_predicted_frame`: 1.05%, 3,684,352 calls

Top `only-affected` buckets:

- `ReplicationServer::tick_serialized`: 53.12%, 1800 calls
- `ReplicationServer::find_or_create_quantized_frame`: 5.47%, 3,686,400 calls
- `PredVelocity::serialize_bytes`: 5.47%, 3,661,815 calls
- `init_frame_data`: 3.65%, 11,245,389 calls
- `PredPosition::serialize_bytes`: 3.39%, 3,686,400 calls
- `all_zero`: 2.60%, 43,161,152 calls
- `frame_component_data`: 2.34%, 46,893,816 calls
- Prediction job callback: 1.56%, 3601 calls
- `ReplicationClient::quantize_predicted_entity`: 1.04%, 3,872,589 calls
- `ReplicationClient::compare_predicted_frame`: 0.52%, 3,684,352 calls
- `ReplicationClient::collect_resimulated_prediction_entities`: 0.78%, 1799 calls

Diagnosis:

- Server update volume is still the dominant benchmark cost. The workload
  mutates position for every predicted entity each tick, so the server serializes
  3.68 million entity records and about 86 MB of update data. The profile is
  measuring replication throughput as much as prediction behavior.
- The O(1) history change worked: `remember_baseline` is still called millions
  of times but no longer receives sampled self time in either profile.
- The raw-byte hooks worked: `frame_component_bytes` is no longer a top bucket,
  but `frame_component_data` is now hot because the code still performs many
  checked component-pointer lookups per component per update.
- `all` rollback remains structurally expensive. It requantizes all predicted
  entities across rollback replay frames, so `quantize_predicted_entity`,
  `blend_resim_errors`, and `all_zero` stay hot.
- In `only-affected`, the remaining client-side rollback cost is dominated by
  repeatedly scanning all active entities to collect affected indices and to
  quantize affected entities, plus small scratch allocations for affected
  vectors and original-frame capture.

Proposed next fixes:

- Add a dense `prediction_rollback_entities_` list maintained when
  `queue_prediction_rollback` first marks an entity. This would remove the
  active-entity scan in `collect_resimulated_prediction_entities` and provide a
  direct list for affected quantization.
- Store rollback scratch vectors on `ReplicationClient` and reuse their capacity
  across ticks: resimulated entity indices, affected local entities, and
  original current prediction frames. This avoids repeated allocation and large
  `resize(entities_.size())` work during rollback.
- Change `capture_original_current_predictions` and `blend_resim_errors` to use
  a compact vector of `{entity_index, frame}` pairs instead of a sparse
  `std::vector<QuantizedFrameData>` sized to all known entities.
- Add direct unchecked frame-byte accessors for internal hot loops after the
  archetype, present mask, component index, and frame size have already been
  validated. This would reduce the residual `frame_component_data` cost.
- For `all`, consider a bulk quantize path that walks active predicted entities
  once per frame and writes component bytes directly using cached archetype
  metadata, instead of redoing per-entity/per-component validation.
- For the benchmark itself, add a client-only replay mode or a server update
  throttling mode so prediction rollback changes can be profiled without
  server serialization dominating every run.

## Experiment 24: Dense Prediction Rollback Entity List

Rationale: `only-affected` rollback was scanning every active entity to collect
the entities with pending prediction rollback. This experiment adds a dense
`prediction_rollback_entities_` membership list maintained when rollback is
queued, plus a per-entity rollback-list index for O(1) removal.

Baseline command:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Baseline result:

- `all`: `wall=4.964283`, `server_replication=3.390532`,
  `client_receive=0.571172`, `client_tick=0.865345`,
  `ack_processing=0.098943`
- `only-affected`: `wall=4.401937`, `server_replication=3.392693`,
  `client_receive=0.605479`, `client_tick=0.273683`,
  `ack_processing=0.091934`

Result:

- `all`: `wall=5.315669`, `server_replication=3.615960`,
  `client_receive=0.624974`, `client_tick=0.928239`,
  `ack_processing=0.106379`
- `only-affected`: `wall=4.263536`, `server_replication=3.299090`,
  `client_receive=0.591484`, `client_tick=0.244827`,
  `ack_processing=0.091385`

Conclusion: accepted. The dense list targets `only-affected`, where client tick
time improved by about 10.5%. The `all` run does not use the affected list and
regressed in this single run with server replication noise.

## Experiment 25: Reusable Prediction Rollback Scratch Vectors

Rationale: rollback allocated local vectors each time for resimulated entity
indices, affected ECS entities, and original current-frame captures. This
experiment moves those vectors onto `ReplicationClient` so their capacity is
reused between rollbacks.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=5.499666`, `server_replication=3.734376`,
  `client_receive=0.722906`, `client_tick=0.884199`,
  `ack_processing=0.115320`
- `only-affected`: `wall=4.437698`, `server_replication=3.439518`,
  `client_receive=0.615323`, `client_tick=0.252838`,
  `ack_processing=0.092916`

Conclusion: mixed and kept as a foundation for compact capture storage. This
single run regressed `only-affected` client tick versus Experiment 24
(`0.244827` to `0.252838`), but it removes per-rollback allocation churn and
sets up replacing sparse original-frame capture with compact records.

## Experiment 26: Compact Original Prediction Capture Records

Rationale: rollback error blending captured original current-frame predictions
into a sparse vector sized to all client entities. This experiment changes that
storage to compact `{entity_index, baseline}` records for entities that
actually had a current prediction sample.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=5.017056`, `server_replication=3.508756`,
  `client_receive=0.581076`, `client_tick=0.792078`,
  `ack_processing=0.097757`
- `only-affected`: `wall=4.786550`, `server_replication=3.715519`,
  `client_receive=0.666470`, `client_tick=0.264098`,
  `ack_processing=0.101491`

Conclusion: mixed and kept. The `all` client tick path improved by about 10.4%
versus Experiment 25, likely from avoiding repeated sparse vector resize and
skipping empty capture slots during blending. The `only-affected` run regressed,
with server replication also substantially higher in this sample.

## Experiment 27: Unchecked Internal Frame Byte Accessors

Rationale: after adding raw-byte trait hooks, `frame_component_data` became a
visible hot bucket because prediction compare, prediction error blending, and
predicted quantization were still repeating bounds checks after the archetype
and frame had already been validated. This experiment adds internal unchecked
component-byte accessors and uses them in those hot prediction loops.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=4.988037`, `server_replication=3.563926`,
  `client_receive=0.531187`, `client_tick=0.756939`,
  `ack_processing=0.097669`
- `only-affected`: `wall=4.434586`, `server_replication=3.509141`,
  `client_receive=0.560742`, `client_tick=0.235678`,
  `ack_processing=0.092158`

Conclusion: accepted. Client tick improved for both policies versus Experiment
26, and client receive also dropped. This confirms the residual
`frame_component_data` profile cost was mostly repeated validation in already
validated prediction loops.

## Experiment 28: Bulk Predicted Quantize Helper

Rationale: `all` rollback requantized predicted entities by repeatedly calling
the single-entity quantization path. This experiment added a bulk helper that
walks a supplied dense entity-index list, caches the current archetype pointer,
uses unchecked frame reset/access, and lets `only-affected` quantization walk
the dense rollback entity list instead of active entities.

Commands:

```sh
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy all --report text
build-bench/kage_sync_prediction_stress --entities 2048 --ticks 1800 --latency-frames 10 --misprediction-percent 5 --rollback-policy only-affected --report text
```

Results:

- `all`: `wall=5.231192`, `server_replication=3.667134`,
  `client_receive=0.604054`, `client_tick=0.813438`,
  `ack_processing=0.106750`
- `only-affected`: `wall=4.671549`, `server_replication=3.605876`,
  `client_receive=0.668188`, `client_tick=0.256813`,
  `ack_processing=0.101442`

Conclusion: rejected and reverted. The generalized helper regressed both
policies versus Experiment 27. The likely cause is that the extra helper code
shape and per-entity generic checks outweighed the small amount of validation it
removed after Experiment 27's unchecked frame accessors.

Final accepted-state confirmation after reverting Experiment 28:

- `all`: `wall=5.429979`, `server_replication=3.865692`,
  `client_receive=0.600053`, `client_tick=0.817149`,
  `ack_processing=0.105340`
- `only-affected`: `wall=4.483648`, `server_replication=3.524287`,
  `client_receive=0.566983`, `client_tick=0.264205`,
  `ack_processing=0.091444`

## Trace Viewer Profiling Setup

Rationale: the trace viewer was visibly slow when scrolling. This profiling
pass used the trace viewer's benchmark mode and gprof support to isolate render
hotspots before changing the UI code.

Trace artifact:

- Directory: `/tmp/kage-viewer-profile-trace-small`
- Contents: synthetic client/server trace files generated by
  `/tmp/kage_generate_viewer_trace.cpp`
- Size: about 22 MiB
- Records: `192330`
- Entities: `160`
- Component rows: `640`

Benchmark command:

```sh
/tmp/kage-sync-viewer-gprof-build/kage_sync_trace_viewer --trace-dir /tmp/kage-viewer-profile-trace-small --benchmark --benchmark-report <report.json> --benchmark-frames 120
```

Final gprof command:

```sh
cmake --build /tmp/kage-sync-viewer-gprof-build --target run_kage_sync_trace_viewer_gprof
```

Final gprof artifact: `/tmp/kage_trace_viewer_target_gprof.txt`

## Experiment 29: Trace Viewer Rounded Pill Baseline

Baseline result:

- `load_ms=1279.16`
- `max_rendered_rows=400`
- `max_rendered_cells=96000`
- `total_avg_ms=309.261`, `total_p50_ms=301.898`,
  `total_p95_ms=320.995`, `total_p99_ms=321.737`,
  `total_max_ms=553.234`
- `timeline_avg_ms=301.848`, `timeline_p50_ms=294.833`,
  `timeline_p95_ms=313.946`, `timeline_p99_ms=314.540`,
  `timeline_max_ms=525.993`
- `details_avg_ms=6.191`
- `selection_avg_ms=6.157`

Profile:

- `ImDrawList::AddPolyline`: `26.01%`, `80,640,491` calls
- `ImDrawList::AddConvexPolyFilled`: `16.08%`, `11,520,596` calls
- Many `ImVec2` and draw-list helper calls

Conclusion: the immediate scrolling cost was dominated by drawing every cell as
a rounded pill with per-cell outlines and dither hatching.

## Experiment 30: Trace Viewer Cheap Square Pills

Rationale: replace rounded pills, rounded outlines, and dense dither hatching
with cheaper filled rectangles and minimal starvation/mispredict marks.

Result:

- Report: `/tmp/kage_trace_viewer_square_pills.json`
- `load_ms=1382.94`
- `max_rendered_rows=400`
- `max_rendered_cells=96000`
- `total_avg_ms=83.978`, `total_p50_ms=82.178`,
  `total_p95_ms=87.485`, `total_p99_ms=97.027`,
  `total_max_ms=148.610`
- `timeline_avg_ms=76.699`, `timeline_p50_ms=75.508`,
  `timeline_p95_ms=80.281`, `timeline_p99_ms=84.988`,
  `timeline_max_ms=118.202`
- `details_avg_ms=6.146`
- `selection_avg_ms=6.117`

Conclusion: accepted. This cut total frame construction by about 73%, but the
viewer still drew all `96000` cells every frame, so scrolling remained too
expensive for large traces.

## Experiment 31: Trace Viewer Row and Frame Clipping

Rationale: only draw rows and frame cells visible in the current scroll window.
The full timeline still reserves the same virtual canvas size, but render work
is bounded by the viewport.

Result:

- Report: `/tmp/kage_trace_viewer_clipped.json`
- `load_ms=1261.56`
- `max_rendered_rows=28`
- `max_rendered_cells=1840`
- `total_avg_ms=11.730`, `total_p50_ms=11.214`,
  `total_p95_ms=12.607`, `total_p99_ms=26.657`,
  `total_max_ms=37.142`
- `timeline_avg_ms=4.216`, `timeline_p50_ms=3.868`,
  `timeline_p95_ms=5.185`, `timeline_p99_ms=5.605`,
  `timeline_max_ms=5.809`
- `details_avg_ms=6.340`
- `selection_avg_ms=6.324`

Conclusion: accepted. Viewport clipping cut timeline construction by another
94.5% versus Experiment 30. The next visible cost was the details panel scanning
all records every frame.

## Experiment 32: Trace Viewer Cached Details

Rationale: rebuild the selected-frame details only when the selected cell
changes instead of scanning every trace record during every frame.

Result:

- Report: `/tmp/kage_trace_viewer_details_cached.json`
- `load_ms=1314.24`
- `max_rendered_rows=28`
- `max_rendered_cells=1840`
- `total_avg_ms=5.467`, `total_p50_ms=4.307`,
  `total_p95_ms=6.143`, `total_p99_ms=26.998`,
  `total_max_ms=39.477`
- `timeline_avg_ms=4.173`
- `details_avg_ms=0.234`
- `selection_avg_ms=0.210`

Conclusion: accepted. Details rendering dropped from about `6.34 ms` to
`0.23 ms` average. The remaining benchmark spikes correspond to selection
changes, where one record scan is still required.

## Experiment 33: Trace Viewer Cached Source Metrics and Benchmark Selections

Rationale: source min/max frame ranges, row counts, cell counts, and benchmark
selection candidates were recomputed during rendering or benchmark selection.
Move them into load-time caches so steady-state scrolling avoids these scans.

Result:

- Report: `/tmp/kage_trace_viewer_metadata_cached.json`
- `load_ms=1309.77`
- `max_rendered_rows=28`
- `max_rendered_cells=1840`
- `total_avg_ms=1.281`, `total_p50_ms=0.675`,
  `total_p95_ms=2.445`, `total_p99_ms=6.761`,
  `total_max_ms=12.193`
- `timeline_avg_ms=0.547`, `timeline_p50_ms=0.237`,
  `timeline_p95_ms=1.497`, `timeline_p99_ms=1.838`,
  `timeline_max_ms=1.935`
- `details_avg_ms=0.231`
- `selection_avg_ms=0.211`

Final confirmation after including source metric construction in `load_ms`:

- Report: `/tmp/kage_trace_viewer_target.json`
- `load_ms=1353.90`
- `max_rendered_rows=28`
- `max_rendered_cells=1840`
- `total_avg_ms=1.275`, `total_p50_ms=0.671`,
  `total_p95_ms=2.289`, `total_p99_ms=6.671`,
  `total_max_ms=13.155`
- `timeline_avg_ms=0.544`, `timeline_p50_ms=0.233`,
  `timeline_p95_ms=1.513`, `timeline_p99_ms=1.683`,
  `timeline_max_ms=2.197`
- `details_avg_ms=0.227`
- `selection_avg_ms=0.208`

Final profile:

- Top samples moved out of the scroll/render path and into load/history
  construction.
- `add_cell_state` linear frame-cell lookup accounted for the largest sampled
  load-time cost.
- Residual draw-list cost was small compared with the original baseline.

Conclusion: accepted. Total measured UI construction improved from
`309.261 ms` average to `1.275 ms` average on the same trace, with timeline
construction dropping from `301.848 ms` to `0.544 ms`. The next candidate is
load-time history construction, especially replacing linear per-row frame-cell
lookup in `add_cell_state`.

## Experiment 34: Trace Viewer Efficient Styling Pass

Rationale: the optimized trace viewer used plain rectangles that were fast but
made different frame states too hard to distinguish. This pass keeps dark-mode
styling and the cheap rectangle path, while adding low-cost per-state marks:
top stripe for prediction, bottom stripe for interpolation, diagonal marks for
server/golden frames, check mark for correct prediction, slash for
misprediction, checker marks for starvation, and cross marks for removal.

Commands:

```sh
/tmp/kage-sync-viewer-build/kage_sync_trace_viewer --trace-dir /tmp/kage-viewer-profile-trace-small --benchmark --benchmark-report /tmp/kage_trace_viewer_style_pass.json --benchmark-frames 120
/tmp/kage-sync-viewer-gprof-build/kage_sync_trace_viewer --trace-dir /tmp/kage-viewer-profile-trace-small --benchmark --benchmark-report /tmp/kage_trace_viewer_style_pass_gprof_build.json --benchmark-frames 120
```

Normal build result:

- `load_ms=324.792`
- `max_rendered_rows=24`
- `max_rendered_cells=1600`
- `total_avg_ms=0.820`, `total_p50_ms=0.561`,
  `total_p95_ms=1.364`, `total_p99_ms=1.788`,
  `total_max_ms=10.536`
- `timeline_avg_ms=0.196`, `timeline_p50_ms=0.058`,
  `timeline_p95_ms=0.606`, `timeline_p99_ms=0.716`,
  `timeline_max_ms=0.745`

Gprof build result:

- `load_ms=1300.55`
- `max_rendered_rows=24`
- `max_rendered_cells=1600`
- `total_avg_ms=1.301`, `total_p50_ms=0.745`,
  `total_p95_ms=2.066`, `total_p99_ms=6.886`,
  `total_max_ms=14.406`
- `timeline_avg_ms=0.491`, `timeline_p50_ms=0.223`,
  `timeline_p95_ms=1.358`, `timeline_p99_ms=1.470`,
  `timeline_max_ms=1.667`

Conclusion: accepted. The styled gprof-build benchmark remains effectively in
line with Experiment 33 (`1.301 ms` total average versus `1.275 ms`) while
making frame classes visually distinct again.
