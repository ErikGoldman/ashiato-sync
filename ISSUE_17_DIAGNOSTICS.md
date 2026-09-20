# Issue 17 diagnostic run

This branch adds correlation data to the existing trace stream. It does not
change the wire format. The diagnostic path is compiled only when tracing is
enabled; packet lifecycle records additionally require packet logs.

Build both the server and client with:

```sh
-DASHIATO_SYNC_ENABLE_TRACING=ON
-DASHIATO_SYNC_TRACE_PACKET_LOGS=ON
```

At runtime, enable the tracer and packet logs on both processes. Keep the same
monitored-client filter, bandwidth settings, entity priorities, socket setup,
and test duration for both sides of the A/B run.

Please run the known-good revision and this branch with the same scenario, then
return the raw server and client trace directories from both runs. Also include
the exact commit, build type, CMake tracing options, packet-ID limit, MTU,
bandwidth settings, client ID, and the wall-clock start time of each process.

## Correlation fields

For `ComponentSent` and `ComponentReceived`, `data` now includes:

- `packet_id` and `record_index`, which identify the exact update record;
- `packet_frame`, `record_kind`, and `baseline_frame`;
- on the client, `changed_sync_slots` and `component_apply_mask`;
- on the server, `stage=transport_submit` after the record has survived packet
  fitting and the packet ID has been allocated.

Packet logs additionally include:

- `server_update_transport_returned` after the server transport callback
  returns;
- one `server_update_record` at client decode and another after apply;
- `ack_queued` and `pending_ack_count` on the client update summary;
- an outcome for every ACK received by the server: `accepted`,
  `packet_not_pending`, or `record_rejected`;
- `server_pending_ack_removed` when pending ACK state is discarded because its
  records became stale, its packet ID was reused, or the pending limit was hit;
- an explicit client `header_decode` failure for truncated update headers.

## How to classify a missing component event

Start with a server `ComponentSent` from an affected entity and follow its
`packet_id` and `record_index`:

1. No `server_update_transport_returned`: the server transport callback threw
   or did not return.
2. No matching client update packet: loss or rejection occurred between the
   server callback and client update dispatch.
3. Client packet has `applied=false`: `apply_failure` identifies the record and
   decode/apply failure.
4. Client record has `stage=decoded` but no `ComponentReceived`: compare the
   component ID, wire ID, changed slots, and trace filters.
5. `ComponentReceived` exists but there is no `stage=applied`: the mode apply
   failed after decode.
6. Both stages exist but `component_apply_mask` excludes the component: the
   snap write-elision path intentionally skipped the registry write. Compare
   the authoritative value and baseline in the trace.
7. Client says `ack_queued=true` but there is no outgoing ACK containing that
   packet ID: the ACK remained queued or packet construction did not drain it.
8. The client emitted the ACK but the server did not log it: loss occurred on
   the return path or before server dispatch.
9. The server reports `packet_not_pending`: look for the corresponding
   `server_pending_ack_removed` reason. `record_rejected` means the packet was
   still tracked but at least one entity/frame ACK was no longer valid.

When sharing a smaller excerpt, include every event for the affected entity and
packet ID on both processes, plus roughly 100 server frames before and after it.
