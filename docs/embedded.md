# Embedded development

Zephyr, LMS and Bluetooth are the primary delivery path. Linux runs portable
algorithm/protocol tests and optional listening tools.

## Implemented and tested

- Zephyr module/Kconfig integration and a pinned `west.yml` (Zephyr v4.3.0).
- Static pools for worker threads, stacks and binary semaphores. Exhaustion
  returns an error; no heap fallback. The same portable C11 atomic implementation
  is built for both operating systems and rejects targets without lock-free
  32-bit atomics. Configure audio-owner priority above decode workers.
- Stereo Q1.31 Zephyr I2S TX adapter: four statically allocated, 32-byte-aligned
  slab buffers, deferred hardware START after the first enqueue, cache flush,
  ownership transfer only after successful write, and DROP on stop. Its initial
  clock configuration is I2S master; PLL tuning and TDM/2.1 output are not present.
- Bounded SlimProto framing/parser and HELO encoding, independent of sockets.
- Bounded SBC-media ingress queue and decoder worker, with an optional real OI
  SBC decoder shared by Linux and Zephyr.
- TCP HAL and bounded LMS control/HTTP raw-PCM client for both platforms.

`platform/zephyr_smoke` runs with `CONFIG_HEAP_MEM_POOL_SIZE=0`. It exercises
kernel threads/semaphores, reservoir EOF, null output, protocol ingress, and a
**test-only** I2S device. Ten I2S start/write/stop cycles alternate success and
write failure, verifying that buffers return to the slab rather than leaking.
The fake device models ownership; it does not model DMA latency or clock drift.

The current checkout and Python build dependencies were prepared under
`/tmp/eaf-zephyr` and `/tmp/eaf-zephyr-venv`. The actual Zephyr commit used was
`3568e1b6d5cdd51a6b964a2a1d6d29200fea2056` (v4.3.0).

To rerun the already-configured build:

```sh
cmake --build build-zephyr
./build-zephyr/zephyr/zephyr.exe -stop_at=1
```

Expected console line: `EAF smoke PASS (0)`.

To configure against a Zephyr installation (host toolchain suffices for native_sim):

```sh
ZEPHYR_BASE=/path/to/zephyr cmake -S platform/zephyr_smoke -B build-zephyr \
  -DBOARD=native_sim/native/64 -DZEPHYR_TOOLCHAIN_VARIANT=host \
  -DPython3_EXECUTABLE=/path/to/zephyr-venv/bin/python
cmake --build build-zephyr
```

Use a fresh build directory when switching Zephyr installations or toolchains.
The sample adds EAF as an extra module automatically. Existing west workspaces
can instead run `west build -b native_sim/native/64 /path/to/eaf/platform/zephyr_smoke`.
The root manifest provides the version pin for a future complete board workspace;
only Zephyr itself was needed for this host-toolchain smoke run. Python build
requirements came from Zephyr's `scripts/requirements-base.txt`. A physical MCU
build additionally needs its toolchain and imported vendor modules.

## I2S board integration

Enable `CONFIG_I2S=y` and `CONFIG_EAF_I2S=y`. Configure the actual I2S device,
pinctrl and codec in the board overlay/setup. Before graph configure, call
`eaf_zephyr_i2s_bind()` with the configured Zephyr device name and use
`eaf_zephyr_i2s_sink` in the static graph. The supplied sink is a singleton.
The graph must output stereo FL/FR at the selected rate, with no more than
`CONFIG_EAF_I2S_MAX_FRAMES` per block. The driver must support 32-bit stereo I2S.

The slab is fixed at build time. Acquiring a block circulates preallocated DMA
storage; it does not grow or allocate a heap pool. Slab and driver-queue waits
are bounded to 20 ms. Actual audio deadlines require board tuning. Internal RAM
placement, DMA reachability and cache maintenance must be verified for the
chosen SoC; alignment alone is insufficient. Device initialization, pin/codec
setup, actual output drain at EOF, xrun recovery and hardware clock slew remain
board work. The current STOP drops queued hardware audio; use live-stream tests
until a hardware drain contract exists.

[I2S ownership and trigger API](https://docs.zephyrproject.org/4.3.0/doxygen/html/group__i2s__interface.html)

## LMS boundary

The transport task feeds TCP bytes to `eaf_lms_feed`. It handles fragmented or
coalesced incoming messages with a two-byte big-endian body length and a 2,048
byte fixed limit. Invalid lengths or callback errors poison the parser until
reset/reconnect. Callbacks borrow parser memory and must copy anything queued to
another thread. Unknown opcodes are delivered to the callback for policy there.

`eaf_lms_parse_stream` extracts raw `strm` fields, server address/port and bounded
request bytes. The PCM fields retain their protocol encoding; this API does not
interpret them as sample rates or automatically issue requests. `eaf_lms_helo`
encodes the different outgoing opcode/32-bit-length framing and caller-supplied
capabilities. Do not advertise codecs, synchronization or scheduling features
that the application does not implement.

`eaf_lms_client` now owns separate control and HTTP sockets through the TCP HAL.
Call init/connect/step/close from one transport worker. Connect uses numeric IPv4
and a bounded 500 ms wait; subsequent socket I/O is nonblocking. Its fixed buffers
retain partial requests, headers, PCM frames and reservoir writes. Control ping
handling continues under PCM backpressure. The start/stop callbacks must arrange
quiescent graph lifecycle changes; the PCM callback returns accepted frames and
the EOF callback publishes source EOF. Allocate the client outside a small stack.

The current subset accepts `strm s` with autostart 0–3 and raw mono/stereo
16/24/32-bit PCM, handles stop/flush and timestamp pings, and emits HELO, RESP,
STMc, STMf, STMl, STMr, STMs, STMt and STMd. HTTP 200 bodies support Content-Length or connection
close. Truncated bodies, chunked transfer, content encoding, ICY metadata and TLS
requests are rejected. PCM is converted to canonical Q1.31 with endian handling.
The caller explicitly reconnects after errors.

This remains a partial LMS implementation.
Modes 2/3 require `cont` after RESP; modes 0/2 emit STMl when the first PCM chunk
(or empty-body EOF) is ready and hold PCM until `strm u` with timestamp zero.
Only one private decoded chunk is retained while waiting. STMl currently means
that bounded chunk is ready, not that the server's requested buffering threshold
has been met. A `cont` with metadata or looping is rejected. Timed starts remain unsupported. Immediate pause/resume uses the optional
`pause(ctx, paused)` callback, which must synchronously acknowledge the output
owner transition and retain queued PCM. Applications without it reject these
commands. Timed pauses are rejected.

The output owner can transfer an `eaf_lms_playback_t` snapshot to the transport
worker, which calls `eaf_lms_client_report_playback`. This emits STMs once per
stream when `started` is true, then STMt with elapsed milliseconds and output
buffer byte counts. The application must provide coherent snapshots and choose
a reporting interval; do not call this API from the audio thread or infer actual
playback from PCM acceptance. Status pings include the latest snapshot. Snapshot
state resets on stream replacement/stop. The host test uses a simulated output
owner; board output timing is not implemented by this API. The native LMS harness
now publishes snapshots after its timed null sink commits.

Remaining work includes embedded output-owner integration,
stream/output thresholds,
replay gain, discovery, automatic reconnect, WAV/AIFF framing, FLAC, scheduled
starts and multi-room sync. STMd means input decode completion, not speaker drain.
Live Lyrion 9.1.1 PCM startup and immediate pause/resume have been tested with
the native null-output harness; audible output and full interoperability remain
unverified.

Wire-layout references: [Squeezelite protocol definitions](https://github.com/ralph-irving/squeezelite/blob/master/slimproto.h)
and [LMS server](https://github.com/LMS-Community/slimserver).

## Bluetooth boundary

`eaf_bt_sbc_receive` accepts an SBC media header and encoded payload **after RTP
header removal**, plus sequence and timestamp. Zephyr 4.3.0's A2DP stream receive
callback provides this boundary. The callback must be the sole serialized
producer and must not decode or block on the PCM reservoir.

Eight fixed packets, each up to 1,024 encoded bytes, are queued for a single
worker. Full queues reject the new packet; malformed/fragmented SBC payloads are
rejected; the next accepted packet carries discontinuity. Sequence rollover is
handled modulo 16 bits. Copies retain no borrowed Bluetooth stack buffers.
Only a stopped producer may expose its diagnostic counters to another thread.
`eaf_bt_decoder_step` runs in the sole PCM producer worker. It decodes at most
one SBC frame per step, preserves partial reservoir writes, resets codec history
on discontinuity, and checks decoded rate/channel format against the reservoir.
The optional OI adapter validates frame CRCs and upmixes mono to stereo before
Q1.31 conversion. A corrupt frame rejects the rest of that packet; earlier valid
frames may already have been published. A zero return can mean backpressure or
an empty queue; arrange worker wakeups rather than spinning at audio priority.
A fixed-rate Zephyr Classic A2DP endpoint and negotiation binding is now
implemented; see [Bluetooth integration](bluetooth.md). Pairing, radio enablement
and board application integration remain work.

Classic A2DP requires a BR/EDR controller supported by the chosen Zephyr board.
LE Audio requires a different profile and LC3 path. Board/controller and desired
phone interoperability must be selected before wiring these adapters; a device
with BLE support alone is not sufficient evidence of A2DP support.

[Zephyr A2DP APIs](https://docs.zephyrproject.org/4.3.0/doxygen/html/group__bt__a2dp.html)
and [current A2DP sample requirements](https://docs.zephyrproject.org/latest/samples/bluetooth/classic/a2dp_sink/README.html).

## Building the real SBC path

The optional dependency is Zephyr's Apache-2.0 OI/libsbc decoder, revision
`8e1beda02acb8972e29e6edbb423f7cafe16e445`, imported by the pinned Zephyr manifest.
The host build does not fetch it automatically:

```sh
cmake -S . -B build-bt-lms -DEAF_LIBSBC_ROOT=/path/to/libsbc
cmake --build build-bt-lms
ctest --test-dir build-bt-lms --output-on-failure
```

For the extended Zephyr smoke configuration:

```sh
ZEPHYR_BASE=/path/to/zephyr cmake -S platform/zephyr_smoke -B build-zephyr-bt-lms \
  -DBOARD=native_sim/native/64 -DZEPHYR_TOOLCHAIN_VARIANT=host \
  -DPython3_EXECUTABLE=/path/to/zephyr-venv/bin/python \
  '-DZEPHYR_MODULES=/path/to/eaf;/path/to/libsbc' \
  -DEXTRA_CONF_FILE=network_bt.conf
cmake --build build-zephyr-bt-lms
./build-zephyr-bt-lms/zephyr/zephyr.exe -stop_at=1
```

This enables `EAF_LMS_CLIENT` and `EAF_BT_SBC`. It compiles the socket adapter and
runs real SBC decoding plus the existing kernel/mock-I2S smoke. It does not open
an LMS connection on Zephyr or exercise a Bluetooth controller.

`cmake/sbc_fixups.cmake` generates build-local copies of three vendor synthesis
files to replace negative signed left shifts with unsigned bit operations,
preserving two's-complement fixed-point behavior. The external checkout and its
license headers are unchanged. It also supplies the toolchain header missing
from Zephyr 4.3's libsbc wrapper. These fixes are tied to the dependency revision;
other revisions need review. EAF retains strict warnings; host vendor sources
are compiled separately with vendor diagnostics disabled.

## squeezelite-esp32 reference

Reviewed the local checkout at
`/home/snappy/Desktop/Projects/squeezelite-esp32/squeezelite-esp32`, commit
`1d542bd53cf397661f11c0671553422acdb663ab`. Relevant references are
`components/squeezelite/{slimproto.c,slimproto.h,stream.c,pcm.c}` and
`components/driver_bt/bt_app_sink.c`.

The reusable guidance is the SlimProto wire layout and lifecycle: autostart and
cont gates, timestamp echo, HTTP response forwarding, and separate decode versus
actual-output status. The Bluetooth implementation shows deferred control events
and stream-rate handling. Its ESP-IDF A2DP data callback receives decoded PCM;
Zephyr's encoded SBC boundary requires the decoder implemented here. ESP-IDF and
FreeRTOS bindings cannot be dropped directly into the Zephyr HAL.

No Squeezelite source was copied into EAF. Protocol behavior informed the new
implementation; the optional decoder is the separately licensed Zephyr module.
Before importing any reference code directly, retain its per-file license and
attribution and review the resulting distribution requirements.

## Live server harness

```sh
cmake --build build-bt-lms
./build-bt-lms/eaf_lms_play 192.168.11.132 300
```

The optional second argument is runtime in seconds (default 60, maximum 86400).
SIGINT/SIGTERM shut down the worker and sockets. The executable connects to
SlimProto TCP port 3483; port 9000 is the server UI, not the audio control port.
Select the registered SqueezePlay player, ID `02:ea:f0:00:00:01`, in the UI and
start a track. Run only one instance with this fixed test ID. The client advertises
PCM only, so the server must supply supported raw PCM or transcode into it.

This harness uses a separate audio thread and a fixed reservoir. With no device
argument it uses inaudible timed null output. Append `default` (or an ALSA PCM
name) to enable audio; see [Linux audio](linux-audio.md). LMS `audg` now controls
stereo master gain/mute through an atomic handoff to the audio owner. Stream changes join the old worker before
resetting the graph. Pause acknowledges at an output block boundary; resume shifts
the null-sink deadline to avoid trying to catch up through the pause interval.
Output snapshots report committed source frames, excluding startup silence.
The transport prints bounded command metadata and stream/underrun diagnostics.

Tested against the user's Lyrion Music Server 9.1.1: player registration,
44.1 kHz stereo PCM stream, pause with a stationary server position, resume,
and stop. The observed stopped stream consumed 306,176 frames with zero underruns.
The test player was stopped after testing. Server-side seek offsets and elapsed
position accuracy, end-of-track advancement,
automatic reconnect and long-run stability still require work. Do not infer
speaker timing or audible quality from these results.
