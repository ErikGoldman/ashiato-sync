# Ashiato Sync Examples

This directory contains interactive examples for exercising the sync library under
local UDP transport, simulated network conditions, prediction, buffered
interpolation, and bandwidth control.

Build the examples with:

```sh
cmake -S . -B build-examples -DASHIATO_SYNC_BUILD_EXAMPLES=ON
cmake --build build-examples --target ashiato_sync_balls_example
cmake --build build-examples --target ashiato_sync_fps_example
```

The executable paths are usually:

```sh
./build-examples/examples/ashiato_sync_balls_example
./build-examples/examples/fps/ashiato_sync_fps_example
```

## Network Simulation

Both examples use `examples/network_simulator.hpp` to simulate local network
conditions before packets are written to UDP sockets.

- `--latency-ms N`: one-way simulated latency in milliseconds.
- `--jitter-ms N`: uniform `+/-N ms` latency jitter.
- `--loss-percent N`: packet loss percentage.
- `--link-bandwidth-kbps N`: simulated link bandwidth. `0` means unlimited.
- `--link-queue-kb N`: simulated link queue size. Packets are dropped when the
  queue is full.

The link simulator is separate from the server bandwidth controller. To test
bandwidth discovery, cap the link with `--link-bandwidth-kbps`. To remove
simulated link bandwidth as a variable, use `--link-bandwidth-kbps 0`.

## Server Bandwidth

Both examples expose the server-side replication bandwidth settings.

- `--bandwidth-mode dynamic|static`: enables dynamic bandwidth control or a
  fixed per-tick send budget.
- `--bandwidth-limit-kbps N`: fixed send budget used when bandwidth mode is
  `static`.
- `--bandwidth-min-kbps N`: dynamic controller minimum.
- `--bandwidth-initial-kbps N`: dynamic controller starting target.
- `--bandwidth-max-kbps N`: dynamic controller maximum.

For effectively unlimited bandwidth in the examples:

```sh
--link-bandwidth-kbps 0 --bandwidth-mode static --bandwidth-limit-kbps 16000
```

## Tracing

Tracing flags require a build with tracing enabled. Packet log tracing requires
`ASHIATO_SYNC_TRACE_PACKET_LOGS`.

- `--trace-dir DIR`: write a trace capture to `DIR`.
- `--trace-frame-data on|off`: include per-frame component data.
- `--trace-packet-logs on|off`: include packet-level logs.

## Balls Example

`ashiato_sync_balls_example` runs a local server and one local client in a single
process. It spawns moving replicated balls, expires and replaces them over time,
and renders the client view with raylib. It is useful for testing replication
load, entity churn, interpolation, prediction, packet loss, and bandwidth limits.

Basic run:

```sh
./build-examples/examples/ashiato_sync_balls_example
```

Buffered interpolation with unlimited simulated bandwidth:

```sh
./build-examples/examples/ashiato_sync_balls_example \
  --client-mode buffered-interpolation \
  --link-bandwidth-kbps 0 \
  --bandwidth-mode static \
  --bandwidth-limit-kbps 16000
```

Balls-specific flags:

- `--client-mode snap|buffered-interpolation|predict`: client replication mode.
- `--entities N`: initial target ball count.
- `--auto-buffered-frame-lag on|off`: enable or disable adaptive buffered frame lag.
- `--buffered-time-dilation-min N`: minimum buffered time dilation.
- `--buffered-time-dilation-max N`: maximum buffered time dilation.
- `--buffered-time-dilation-gain N`: gain used when correcting buffered playback error.

Runtime controls:

- `M`: cycle snap, buffered interpolation, and prediction.
- `1`, `2`, `3`: switch directly to snap, buffered interpolation, or prediction.
- `Up` / `Down`: change target entity count by 8.
- `Shift+Up` / `Shift+Down`: change target entity count by 1.
- `PageUp` / `PageDown`: change target entity count by 32.
- `Home` / `End`: set target entity count to max or min.
- `[` / `]`: decrease/increase latency.
- `;` / `'`: decrease/increase jitter.
- `-` / `=`: decrease/increase packet loss.

## FPS Example

`ashiato_sync_fps_example` is a small first-person networked game. It can run as a
dedicated server, a client, a listen server, or a launcher that starts a server
and multiple local clients. It exercises client prediction, buffered remote
entities, input replication, cues, replay recording, and simulated links.

Run a dedicated server and a separate client:

```sh
./build-examples/examples/fps/ashiato_sync_fps_example --server --port 37043 --bots 4
./build-examples/examples/fps/ashiato_sync_fps_example --client --host 127.0.0.1 --port 37043
```

Run a listen server:

```sh
./build-examples/examples/fps/ashiato_sync_fps_example --listen --clients 1 --bots 4
```

Run a local launcher that starts one server and multiple clients:

```sh
./build-examples/examples/fps/ashiato_sync_fps_example --clients 2 --bots 4
```

FPS mode flags:

- `--server`: run a dedicated server.
- `--client`: run a standalone client.
- `--listen`: run a listen server with a local playable client.
- `--clients N`: launcher mode; start a server plus `N` local clients.

FPS configuration flags:

- `--host A.B.C.D`: server address for client or launcher clients. Default:
  `127.0.0.1`.
- `--port N`: game UDP port.
- `--replay-port N`: replay UDP port. Defaults to `--port + 1`.
- `--bots N`: number of server-controlled bots.
- `--replay-dir DIR`: replay frame output directory.

The FPS example also accepts the shared network simulation, bandwidth, and
tracing flags listed above.
