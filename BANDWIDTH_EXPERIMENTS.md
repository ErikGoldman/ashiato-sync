# Bandwidth Experiments

Benchmark scenario unless noted:

```sh
./build-bench/ashiato_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

The `build-bench` configuration uses `RelWithDebInfo` and
`ASHIATO_SYNC_BUILD_BENCHMARKS=ON`.

Artifact: `build-bench/ashiato_sync_ball_stress`

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

## Experiment 4: Adaptive Packet ID Width

Rationale: packet-level ACKs made upload ACK traffic small, but each ACK record
and each server update header still carried a fixed 32-bit packet id. The
server only retains a bounded pending packet ACK window, so this experiment
derives the packet id bit width from `max_pending_packet_acks_per_client`. The
default window is `255`, which uses 8-bit packet ids and abandons stale pending
packet ACK records before id reuse.

Result:

- `wall=12.165908`
- `server_replication=7.844172`
- `client_receive=3.601972`
- `ack_processing=0.547028`
- `server_to_clients.bytes=219606978`
- `clients_to_server.bytes=109362`
- `clients_to_server.ack_records=98601`
- `clients_to_server.ack_record_bits=788808`
- `server_to_clients.server_update_header_bits=11965312`
- `server_to_clients.full_upsert_bits=239743217`
- `server_to_clients.delta_upsert_bits=1502872601`
- `server_to_clients.destroy_record_bits=1620366`

Conclusion: accepted for upload ACK compression. ACK record bits dropped from
`5469280` to `788808` and upload bytes dropped from `694421` to `109362`.
Downstream bytes increased in this run because lower upload pressure changed
which packets landed in the lossy stress simulation, so this should be compared
primarily by direct ACK/header bit diagnostics rather than total downstream
bytes alone.

## Experiment 5: Tiered Network Entity ID Width

Rationale: most stress-test network ids are small, but update and destroy
records still used a fixed 32-bit network id. This experiment encodes ids in
tiers: 16 bits for ids below `2^15`, 25 bits below `2^23`, and 34 bits for the
full 32-bit range.

Result:

- `wall=12.726862`
- `server_replication=8.228117`
- `client_receive=3.785806`
- `ack_processing=0.555434`
- `server_to_clients.bytes=207773924`
- `clients_to_server.bytes=105171`
- `clients_to_server.ack_records=94407`
- `clients_to_server.ack_record_bits=755256`
- `server_to_clients.server_update_header_bits=11322752`
- `server_to_clients.full_upsert_bits=261932080`
- `server_to_clients.delta_upsert_bits=1387486201`
- `server_to_clients.destroy_record_bits=831181`

Conclusion: accepted. Delta-upsert bits dropped from `1502872601` to
`1387486201`, destroy bits dropped from `1620366` to `831181`, and downstream
bytes dropped from `219606978` to `207773924`. Full-upsert bits rose in this
particular lossy run because more fulls were resent, but per-record id overhead
is lower for all ids in the common small-id tier.

## Experiment 6: Full-Upsert Presence Masks

Rationale: compact full-upsert slot indices still pay one slot id per present
component. Dense full upserts can be smaller if the packet writes a fixed
archetype-local presence mask and then streams payloads in slot order. This
experiment adds a one-bit full-upsert mode selector and chooses the mask when it
is smaller than the slot-list encoding.

Result:

- `wall=11.595590`
- `server_replication=7.596421`
- `client_receive=3.425211`
- `ack_processing=0.429267`
- `server_to_clients.bytes=206490083`
- `clients_to_server.bytes=101570`
- `clients_to_server.ack_records=90809`
- `clients_to_server.ack_record_bits=726472`
- `server_to_clients.full_upsert_bits=250704306`
- `server_to_clients.full_upsert_payload_bits=164110746`
- `server_to_clients.full_upsert_slot_list_records=0`
- `server_to_clients.full_upsert_presence_mask_records=721613`
- `server_to_clients.full_upsert_presence_mask_bits=3608065`
- `server_to_clients.delta_upsert_bits=1388550699`
- `server_to_clients.destroy_record_bits=856290`

Conclusion: accepted. Full-upsert bits dropped from `261932080` to
`250704306` despite more full records in the run, and downstream bytes dropped
from `207773924` to `206490083`. The stress archetype is dense enough that all
full upserts selected presence-mask mode.

## Experiment 7: Reuse ACKed Global Network IDs

Rationale: tiered entity-id encoding works best while network ids stay in the
small-id tier. This experiment keeps ids server-global, but recycles a destroyed
entity's id once every connected client has ACKed the destroy or disconnected.
The client also keeps destroy tombstones so delayed full packets for the old
server entity cannot recreate it after the id is reused.

Result:

- `wall=11.237633`
- `server_replication=7.392518`
- `client_receive=3.285498`
- `ack_processing=0.408407`
- `server_to_clients.bytes=206446790`
- `clients_to_server.bytes=101423`
- `clients_to_server.ack_records=90662`
- `clients_to_server.ack_record_bits=725296`
- `server_to_clients.full_upsert_bits=249983530`
- `server_to_clients.delta_upsert_bits=1388931774`
- `server_to_clients.destroy_record_bits=852244`

CPU comparison against Experiment 6:

- `wall`: `11.595590` -> `11.237633`
- `server_replication`: `7.596421` -> `7.392518`
- `client_receive`: `3.425211` -> `3.285498`
- `ack_processing`: `0.429267` -> `0.408407`

Conclusion: accepted. The optimized reclaim path did not show a CPU regression
in the canonical stress run, and total measured traffic changed only slightly.

## Experiment 8: Dedicated Ping Timing Packets

Rationale: update-arrival timing couples interpolation latency/jitter estimates
to replication cadence and loss. This experiment adds dedicated ping/pong
control packets on a configurable cadence, defaulting to 3 seconds, and uses
RTT/2 ping samples for client latency/jitter. Server update receive still tracks
measured interpolation buffer depth, but no longer changes latency/jitter.

Command:

```sh
./build-bench/ashiato_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

Artifact: `build-bench/ashiato_sync_ball_stress`

Result:

- `wall=11.818390`
- `server_replication=7.649816`
- `client_receive=3.573520`
- `ack_processing=0.450216`
- `server_to_clients.bytes=206159340`
- `clients_to_server.bytes=101468`
- `server_to_clients.server_pong_packets=40`
- `server_to_clients.server_pong_bytes=360`
- `clients_to_server.client_ping_packets=40`
- `clients_to_server.client_ping_bytes=360`
- `clients_to_server.ack_records=90341`
- `clients_to_server.ack_record_bits=722728`

CPU comparison against Experiment 7:

- `wall`: `11.237633` -> `11.818390`
- `server_replication`: `7.392518` -> `7.649816`
- `client_receive`: `3.285498` -> `3.573520`
- `ack_processing`: `0.408407` -> `0.450216`

Conclusion: accepted with follow-up watch item. Ping control traffic is tiny
(`720` total ping/pong bytes in this run), and total traffic stayed effectively
flat. CPU timings are higher than Experiment 7 in this single canonical run, so
repeat measurements should be taken before attributing the delta to ping logic
rather than benchmark variance or the changed interpolation timing target.

## Experiment 9: Client-Local Wire IDs with ClientEntityNetworkId

Rationale: server-global network ids grow with total replicated lifetime and
make the compact-id tiers less effective. This experiment makes the over-the-wire
entity id client-local, reuses ids after that client ACKs the destroy, and has
the client immediately expand the compact wire id to a 64-bit
`ClientEntityNetworkId` containing client id, wire id, and implicit version. Full
upserts no longer serialize the server ECS entity id.

Command:

```sh
./build-bench/ashiato_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

Artifact: `build-bench/ashiato_sync_ball_stress`

Profile artifact: `/tmp/ashiato_sync_client_local_wire_ids_gprof.txt` from
`build-bench-gprof/ashiato_sync_ball_stress`

Result:

- `wall=12.762600`
- `server_replication=8.176837`
- `client_receive=3.953091`
- `ack_processing=0.485833`
- `server_to_clients.bytes=200414848`
- `clients_to_server.bytes=103028`
- `server_to_clients.full_upsert_bits=198771664`
- `server_to_clients.delta_upsert_bits=1392255400`
- `server_to_clients.destroy_record_bits=820505`
- `clients_to_server.ack_records=91907`
- `clients_to_server.ack_record_bits=735256`

CPU comparison against Experiment 8:

- `wall`: `11.818390` -> `12.762600`
- `server_replication`: `7.649816` -> `8.176837`
- `client_receive`: `3.573520` -> `3.953091`
- `ack_processing`: `0.450216` -> `0.485833`

Conclusion: bandwidth win with CPU follow-up needed. Downstream bytes dropped
by about `5.74 MB` versus Experiment 8, and full-upsert bits dropped by about
`51.9 Mbits`, which is the intended effect of removing the 64-bit server entity
from full upserts and keeping wire ids dense per client. CPU timings are higher
in this single run. The gprof flat profile still points primarily at the
existing replication path (`tick_serialized`, quantized-frame lookup, client
upsert/baseline work); the new client-id lookup helpers are visible but small
(`find_entity_state(unsigned long)` sampled at `0.02s` self).

## Experiment 10: 8-bit Lowest Network ID Tier

Rationale: client-local wire ids make low values more common, so this experiment
changes the first network-id tier from 15 payload bits plus an extension bit to
8 payload bits plus an extension bit. IDs `1..255` encode in `9` bits instead
of `16`; IDs `256..8388607` encode in `25` bits.

Command:

```sh
./build-bench/ashiato_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

Artifact: `build-bench/ashiato_sync_ball_stress`

Result:

- `wall=12.660091`
- `server_replication=8.329575`
- `client_receive=3.661622`
- `ack_processing=0.503279`
- `server_to_clients.bytes=208561525`
- `clients_to_server.bytes=99172`
- `server_to_clients.full_upsert_bits=214940513`
- `server_to_clients.delta_upsert_bits=1440379227`
- `server_to_clients.destroy_record_bits=1198542`
- `clients_to_server.ack_records=88051`
- `clients_to_server.ack_record_bits=704408`

Bandwidth comparison against Experiment 9:

- `server_to_clients.bytes`: `200414848` -> `208561525`
- `server_to_clients.full_upsert_bits`: `198771664` -> `214940513`
- `server_to_clients.delta_upsert_bits`: `1392255400` -> `1440379227`
- `server_to_clients.destroy_record_bits`: `820505` -> `1198542`

Conclusion: rejected for this workload. The stress test has thousands of live
entities per client, so most entity records use ids above `255`. Those ids moved
from the old 16-bit tier to the 25-bit tier, which outweighed the savings for
the first 255 ids and increased downstream traffic by about `8.15 MB` over the
30-second run.

## Experiment 11: 11-bit Default Network ID Tier

Rationale: the 8-bit tier regressed once more than 255 entities were live per
client. This experiment keeps the tier width configurable on the client and
server and defaults the low tier to 11 bits, so ids `1..2047` encode in `12`
bits while ids above that still use the 25-bit middle tier. The fixed
`ReplicationClient` and `ReplicationServer` types use the default; templated
`ReplicationClientT<bits>` and `ReplicationServerT<bits>` wrappers set the
width from a compile-time argument.

Command:

```sh
./build-bench/ashiato_sync_ball_stress --duration-seconds 30 --clients 4 --max-balls 4096 --spawn-interval-ms 5 --poison-min 1 --poison-max 8 --health-min 20 --health-max 80 --latency-ms 50 --jitter-ms 25 --loss-percent 1 --client-mode buffered-interpolation --interpolation-buffer-frames 2 --time-dilation-min 0.95 --time-dilation-max 1.05 --time-dilation-gain 0.05 --wire-diagnostics --report text
```

Artifact: `build-bench/ashiato_sync_ball_stress`

Result:

- `wall=12.542076`
- `server_replication=7.615497`
- `client_receive=4.169951`
- `ack_processing=0.592262`
- `server_to_clients.bytes=200610468`
- `clients_to_server.bytes=99628`
- `server_to_clients.full_upsert_bits=210966631`
- `server_to_clients.delta_upsert_bits=1381630696`
- `server_to_clients.destroy_record_bits=758550`
- `clients_to_server.ack_records=88507`
- `clients_to_server.ack_record_bits=708056`

Bandwidth comparison:

- vs 15-bit default from Experiment 9: `200414848` -> `200610468`
- vs 8-bit default from Experiment 10: `208561525` -> `200610468`

Conclusion: accepted as the configurable default requested, with a workload
note. It is much better than the 8-bit tier for this stress case, but slightly
worse than the previous 15-bit tier in total downstream bytes because enough
records still use ids above `2047` and pay the 25-bit middle tier.
